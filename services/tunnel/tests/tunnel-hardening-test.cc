#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tunnel-harness.hxx"

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>

using namespace tunnel;
using namespace tunnel::test;

namespace
{

constexpr char kSecret[] = "f5-4-loopback-secret";

std::optional<Frame> firstFrameOf(const std::string& bytes, FrameType type)
{
  FrameParser parser;
  parser.feed(bytes);
  while (parser.hasFrame()) {
    Frame frame = parser.popFrame();
    if (frame.type == type)
      return frame;
  }
  return std::nullopt;
}

std::string payloadOf(const std::string& bytes, FrameType type)
{
  const auto frame = firstFrameOf(bytes, type);
  if (!frame.has_value())
    return {};
  return frame->payload;
}

struct RelayRig
{
  explicit RelayRig(const TunnelMux::Limits& limits)
  {
    RelayOptions options;
    options.host = "127.0.0.1";
    options.devicePort = 0;
    options.homePort = 0;
    options.secret = kSecret;
    options.limits = limits;
    relay = std::make_unique<TunnelRelay>(loop, std::move(options));
    started = relay->start();
    loopThread = std::thread([this] { loop.run(); });
  }

  ~RelayRig()
  {
    relay->stop();
    loop.stop();
    loopThread.join();
    relay.reset();
  }

  std::shared_ptr<TestPeer> dialHome()
  {
    return connectTestPeer(
        {.loop = loop, .ip = "127.0.0.1", .port = relay->homePort()});
  }

  PollLoop loop;
  std::unique_ptr<TunnelRelay> relay;
  std::thread loopThread;
  bool started{false};
};

}

TEST_CASE("a home handshake in progress is not displaced by a stranger")
{
  TunnelMux::Limits limits;
  limits.authTimeout = std::chrono::seconds(1);
  limits.maxPendingHomesPerIp = 4;
  RelayRig rig(limits);
  REQUIRE(rig.started);

  auto home = rig.dialHome();
  REQUIRE(waitFor([&] {
    return firstFrameOf(home->bytes(), FrameType::Challenge).has_value();
  }, 5000));
  const std::string challenge =
      payloadOf(home->bytes(), FrameType::Challenge);

  auto stranger = rig.dialHome();
  REQUIRE(waitFor([&] {
    return firstFrameOf(stranger->bytes(), FrameType::Challenge).has_value();
  }, 5000));
  CHECK_FALSE(home->eof.load());

  const std::string mac = authMac(kSecret, challenge);
  postSend({.loop = rig.loop,
            .peer = home->peer,
            .data = encodeFrame({.type = FrameType::Auth,
                                 .streamId = 0,
                                 .payload = mac.data(),
                                 .size = mac.size()})});
  REQUIRE(waitFor([&] {
    return firstFrameOf(home->bytes(), FrameType::AuthOk).has_value();
  }, 5000));
  CHECK(payloadOf(home->bytes(), FrameType::AuthOk) ==
        relayAuthMac(kSecret, challenge));
  CHECK(waitFor([&] { return rig.relay->hasHome(); }, 5000));

  CHECK(waitFor([&] { return stranger->eof.load(); }, 5000));
  CHECK_FALSE(home->eof.load());
}

TEST_CASE("a wrong AUTH on one handshake leaves the others untouched")
{
  TunnelMux::Limits limits;
  limits.maxPendingHomesPerIp = 4;
  RelayRig rig(limits);
  REQUIRE(rig.started);

  auto liar = rig.dialHome();
  auto honest = rig.dialHome();
  REQUIRE(waitFor([&] {
    return firstFrameOf(liar->bytes(), FrameType::Challenge).has_value() &&
           firstFrameOf(honest->bytes(), FrameType::Challenge).has_value();
  }, 5000));

  const std::string wrong = authMac("not-the-secret", "x");
  postSend({.loop = rig.loop,
            .peer = liar->peer,
            .data = encodeFrame({.type = FrameType::Auth,
                                 .streamId = 0,
                                 .payload = wrong.data(),
                                 .size = wrong.size()})});
  REQUIRE(waitFor([&] { return liar->eof.load(); }, 5000));
  CHECK(firstFrameOf(liar->bytes(), FrameType::AuthFail).has_value());

  const std::string challenge =
      payloadOf(honest->bytes(), FrameType::Challenge);
  const std::string mac = authMac(kSecret, challenge);
  postSend({.loop = rig.loop,
            .peer = honest->peer,
            .data = encodeFrame({.type = FrameType::Auth,
                                 .streamId = 0,
                                 .payload = mac.data(),
                                 .size = mac.size()})});
  CHECK(waitFor([&] { return rig.relay->hasHome(); }, 5000));
  CHECK_FALSE(honest->eof.load());
}

TEST_CASE("one address cannot hold more handshakes than its quota")
{
  TunnelMux::Limits limits;
  limits.maxPendingHomesPerIp = 2;
  RelayRig rig(limits);
  REQUIRE(rig.started);

  auto first = rig.dialHome();
  auto second = rig.dialHome();
  REQUIRE(waitFor([&] {
    return firstFrameOf(first->bytes(), FrameType::Challenge).has_value() &&
           firstFrameOf(second->bytes(), FrameType::Challenge).has_value();
  }, 5000));

  auto third = rig.dialHome();
  CHECK(waitFor([&] { return third->eof.load(); }, 5000));
  CHECK(third->bytes().empty());
  CHECK_FALSE(first->eof.load());
  CHECK_FALSE(second->eof.load());
}

TEST_CASE("one device address cannot take more than its stream quota")
{
  HarnessOptions options;
  options.echoGateway = true;
  options.limits.maxStreamsPerIp = 2;
  Harness harness(std::move(options));
  REQUIRE(harness.start());

  const auto dial = [&harness] {
    return connectTestPeer({.loop = harness.loop,
                            .ip = "127.0.0.1",
                            .port = harness.relay->devicePort()});
  };
  auto first = dial();
  auto second = dial();
  REQUIRE(waitFor([&] {
    return first->connected.load() && second->connected.load() &&
           harness.relay->streamCount() == 2;
  }, 5000));

  auto third = dial();
  CHECK(waitFor([&] { return third->eof.load(); }, 5000));
  CHECK(harness.relay->streamCount() == 2);

  const std::string payload = makePayload(1024, 3);
  postSend({.loop = harness.loop, .peer = first->peer, .data = payload});
  REQUIRE(waitFor([&] { return first->bytes().size() == payload.size(); },
                  5000));
  CHECK(first->bytes() == payload);

  harness.loop.post([peer = first->peer] { peer->close(); });
  REQUIRE(waitFor([&] { return harness.relay->streamCount() == 1; }, 5000));
  auto fourth = dial();
  REQUIRE(waitFor([&] {
    return fourth->connected.load() && harness.relay->streamCount() == 2;
  }, 5000));
  CHECK_FALSE(fourth->eof.load());
  harness.stop();
}

TEST_CASE("a paused listener accepts again after its backoff")
{
  PollLoop loop;
  std::atomic<int> accepted{0};
  TcpListener::Params params;
  params.loop = &loop;
  params.ip = "127.0.0.1";
  params.port = 0;
  params.onAccept = [&accepted](int fd, const std::string&, uint16_t) {
    accepted.fetch_add(1);
    ::close(fd);
  };
  auto listener = TcpListener::create(params);
  REQUIRE(listener);
  std::thread loopThread([&loop] { loop.run(); });

  std::atomic<bool> paused{false};
  loop.post([&] {
    listener->pauseAccepting();
    paused.store(listener->acceptPaused());
  });
  REQUIRE(waitFor([&] { return paused.load(); }, 5000));

  auto peer = connectTestPeer({.loop = loop, .ip = "127.0.0.1",
                               .port = listener->boundPort()});
  CHECK(waitFor([&] { return accepted.load() == 1; }, 5000));
  CHECK(listener->acceptPauses() == 1);
  CHECK_FALSE(listener->acceptPaused());

  loop.stop();
  loopThread.join();
}

TEST_CASE("the client's reconnect delay doubles up to its cap and never stops")
{
  CHECK(reconnectDelayMs({.attempt = 1, .baseMs = 2000, .maxMs = 60000}) == 2000);
  CHECK(reconnectDelayMs({.attempt = 2, .baseMs = 2000, .maxMs = 60000}) == 4000);
  CHECK(reconnectDelayMs({.attempt = 5, .baseMs = 2000, .maxMs = 60000}) == 32000);
  CHECK(reconnectDelayMs({.attempt = 6, .baseMs = 2000, .maxMs = 60000}) == 60000);
  CHECK(reconnectDelayMs({.attempt = 1000, .baseMs = 2000, .maxMs = 60000}) == 60000);
  CHECK(reconnectDelayMs({.attempt = 1, .baseMs = 0, .maxMs = 0}) == 1);
}
