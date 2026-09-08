#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tunnel-harness.hxx"

#include <doctest/doctest.h>

#include <unistd.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

using namespace tunnel;
using namespace tunnel::test;

namespace
{
// Scripted relay stand-in: replays its script on every accepted link and records the client's replies.
struct FakeRelay
{
  std::function<void(TcpPeer&)> script;
  std::unique_ptr<TcpListener> listener;
  TcpPeer::Ptr peer;
  std::atomic<int> acceptCount{0};

  void start(PollLoop& loop)
  {
    TcpListener::Params params;
    params.loop = &loop;
    params.ip = "127.0.0.1";
    params.port = 0;
    params.onAccept = [this, &loop](int fd, const std::string& ip,
                                    uint16_t port) {
      acceptCount.fetch_add(1);
      TcpPeer::Params peerParams;
      peerParams.loop = &loop;
      peerParams.fd = fd;
      peerParams.ip = ip;
      peerParams.port = port;
      TcpPeer::Callbacks callbacks;
      callbacks.onRead = [this](TcpPeer&, const char* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex);
        received.append(data, size);
      };
      callbacks.onEof = [this](TcpPeer& gone) { gone.close(); };
      peerParams.callbacks = std::move(callbacks);
      if (peer && !peer->closed())
        peer->close();
      peer = TcpPeer::adopt(peerParams);
      if (script)
        script(*peer);
    };
    listener = TcpListener::create(params);
    REQUIRE(listener);
  }

  uint16_t port() const { return listener->boundPort(); }

  std::string bytes()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return received;
  }

  std::mutex mutex;
  std::string received;
};

// Counts the gateway dials the client attempts (the exploit's observable).
struct GatewayCounter
{
  std::unique_ptr<TcpListener> listener;
  std::atomic<int> dials{0};

  void start(PollLoop& loop)
  {
    TcpListener::Params params;
    params.loop = &loop;
    params.ip = "127.0.0.1";
    params.port = 0;
    params.onAccept = [this](int fd, const std::string&, uint16_t) {
      dials.fetch_add(1);
      ::close(fd);
    };
    listener = TcpListener::create(params);
    REQUIRE(listener);
  }

  uint16_t port() const { return listener->boundPort(); }
};

// Relay stand-in + client + gateway counter on one PollLoop thread; the 1 s auth timeout re-runs each scenario.
struct RogueRig
{
  explicit RogueRig(const std::function<void(TcpPeer&)>& script)
  {
    relay.script = script;
    relay.start(loop);
    gateway.start(loop);

    ClientOptions options;
    options.relayHost = "127.0.0.1";
    options.relayPort = relay.port();
    options.gatewayHost = "127.0.0.1";
    options.gatewayPort = gateway.port();
    options.secret = "f5-4-loopback-secret";
    options.reconnectWaitMs = 50;
    options.maxReconnects = 100;
    options.limits.authTimeout = std::chrono::seconds(1);
    client = std::make_unique<TunnelClient>(loop, std::move(options));

    loopThread = std::thread([this] { loop.run(); });
    loop.post([&] { client->start(); });
  }

  ~RogueRig()
  {
    client->stop();
    relay.peer.reset();
    relay.listener.reset();
    gateway.listener.reset();
    loop.stop();
    loopThread.join();
  }

  PollLoop loop;
  std::thread loopThread;
  FakeRelay relay;
  GatewayCounter gateway;
  std::unique_ptr<TunnelClient> client;
};

constexpr int kLinkAccepts = 3;
constexpr int kAcceptTimeoutMs = 20000;

std::string authFrame(const std::string& secret, const std::string& challenge)
{
  const std::string mac = authMac(secret, challenge);
  return encodeFrame(FrameType::Auth, 0, mac.data(), mac.size());
}
} // namespace

TEST_CASE("OPEN before any AUTH never dials the gateway")
{
  RogueRig rig([](TcpPeer& peer) {
    peer.send(encodeFrame(FrameType::Open, 5));
  });

  REQUIRE(waitFor([&] {
    return rig.relay.acceptCount.load() >= kLinkAccepts;
  }, kAcceptTimeoutMs));
  CHECK_FALSE(rig.client->homeActive());
  CHECK(rig.client->streamCount() == 0);
  CHECK(rig.gateway.dials.load() == 0);
}

TEST_CASE("PUSH before any AUTH never reaches the intent queue")
{
  RogueRig rig([](TcpPeer& peer) {
    peer.send(encodeFrame(FrameType::Push, 0, R"({"notificationId":1})",
                          18));
  });

  REQUIRE(waitFor([&] {
    return rig.relay.acceptCount.load() >= kLinkAccepts;
  }, kAcceptTimeoutMs));
  CHECK_FALSE(rig.client->homeActive());
  CHECK(rig.client->pushReceived() == 0);
}

TEST_CASE("pipelined OPEN after AUTH_FAIL in one burst never dials")
{
  RogueRig rig([](TcpPeer& peer) {
    std::string burst = encodeFrame(FrameType::AuthFail, 0);
    burst += encodeFrame(FrameType::Open, 6);
    peer.send(burst);
  });

  REQUIRE(waitFor([&] {
    return rig.relay.acceptCount.load() >= kLinkAccepts;
  }, kAcceptTimeoutMs));
  CHECK_FALSE(rig.client->homeActive());
  CHECK(rig.client->streamCount() == 0);
  CHECK(rig.gateway.dials.load() == 0);
}

TEST_CASE("AUTH_OK without a valid relay proof does not activate the link")
{
  const std::string secret = "f5-4-loopback-secret";
  const std::string challenge = randomChallenge();
  RogueRig rig([&](TcpPeer& peer) {
    peer.send(encodeFrame(FrameType::Challenge, 0, challenge.data(),
                          challenge.size()));
    const std::string bogus(kAuthPayloadSize, '\0');
    peer.send(encodeFrame(FrameType::AuthOk, 0, bogus.data(), bogus.size()));
  });

  REQUIRE(waitFor([&] {
    return rig.relay.acceptCount.load() >= kLinkAccepts;
  }, kAcceptTimeoutMs));
  CHECK_FALSE(rig.client->homeActive());
  CHECK(rig.gateway.dials.load() == 0);
  // The client bound its AUTH mac to the per-link challenge.
  CHECK(rig.relay.bytes().find(authFrame(secret, challenge)) !=
        std::string::npos);
}

TEST_CASE("a correct challenge handshake activates the client link")
{
  const std::string secret = "f5-4-loopback-secret";
  const std::string challenge = randomChallenge();
  RogueRig rig([&](TcpPeer& peer) {
    peer.send(encodeFrame(FrameType::Challenge, 0, challenge.data(),
                          challenge.size()));
    const std::string proof = relayAuthMac(secret, challenge);
    peer.send(encodeFrame(FrameType::AuthOk, 0, proof.data(), proof.size()));
  });

  REQUIRE(waitFor([&] { return rig.client->homeActive(); }, 5000));
  CHECK(rig.client->streamCount() == 0);
  CHECK(rig.gateway.dials.load() == 0);
}
