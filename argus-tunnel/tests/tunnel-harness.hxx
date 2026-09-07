#pragma once

#include <client/tunnel-client.hxx>
#include <net/poll-loop.hxx>
#include <net/tcp-listener.hxx>
#include <net/tcp-peer.hxx>
#include <relay/tunnel-relay.hxx>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>

namespace tunnel::test
{
// Accepted gateway-side connection recorded by the stub server.
struct StubConn
{
  TcpPeer::Ptr peer;
  std::string received;
  bool eof{false};
};

// Test-side endpoint (device behind the relay, or the remote app).
struct TestPeer
{
  TcpPeer::Ptr peer;
  std::mutex mutex;
  std::string received;
  std::atomic<bool> connected{false};
  std::atomic<bool> closed{false};
  std::atomic<bool> eof{false};

  std::string bytes()
  {
    std::lock_guard<std::mutex> lock(mutex);
    return received;
  }
  void append(const char* data, size_t size)
  {
    std::lock_guard<std::mutex> lock(mutex);
    received.append(data, size);
  }
};

struct HarnessOptions
{
  std::string secret{"f5-4-loopback-secret"};
  bool echoGateway{false};
  std::string gatewayReply;
  // Rate-limits the stub to one read burst every N ms (0 = full speed).
  // A fully stopped reader makes the kernel close the TCP window and the
  // sender fall into zero-window probe backoff (~75 KB/s), so a slow
  // reader exercises the tunnel valves without that pathology.
  int slowGatewayReadMs{0};
  // Shrinks the stub's kernel receive buffer so back-pressure tests do not
  // depend on loopback autotuning.
  int gatewayRcvBuf{0};
  TunnelMux::Limits limits;
  // Push-intent queue capacities (F5-5).
  size_t relayPushCapacity{256};
  size_t clientPushCapacity{256};
};

inline std::shared_ptr<TestPeer>
connectTestPeer(PollLoop& loop, const std::string& ip, uint16_t port,
                int sndBuf = 0)
{
  auto peer = std::make_shared<TestPeer>();
  TcpPeer::Params params;
  params.loop = &loop;
  params.ip = ip;
  params.port = port;
  params.sndBuf = sndBuf;
  TcpPeer::Callbacks callbacks;
  callbacks.onConnected = [peer](TcpPeer&) { peer->connected.store(true); };
  callbacks.onRead = [peer](TcpPeer&, const char* data, size_t size) {
    peer->append(data, size);
  };
  callbacks.onEof = [peer](TcpPeer&) { peer->eof.store(true); };
  callbacks.onClosed = [peer](TcpPeer&) { peer->closed.store(true); };
  params.callbacks = std::move(callbacks);
  peer->peer = TcpPeer::connect(params);
  return peer;
}

// Sends from the test thread must run on the loop thread next to flush().
inline void postSend(PollLoop& loop, const TcpPeer::Ptr& peer,
                     const std::string& data)
{
  loop.post([peer, data] { peer->send(data); });
}

// Deterministic LCG payload; no library RNG so failures reproduce.
inline std::string makePayload(size_t size, uint32_t seed)
{
  std::string payload;
  payload.reserve(size);
  uint32_t state = seed;
  for (size_t i = 0; i < size; ++i) {
    state = state * 1664525u + 1013904223u;
    payload.push_back(static_cast<char>((state >> 24) & 0xFF));
  }
  return payload;
}

template <typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

// Loopback harness: relay + client + stub gateway on ephemeral ports, all
// in-process over one PollLoop thread.
struct Harness
{
  explicit Harness(HarnessOptions options) : options_(std::move(options)) {}

  ~Harness() { stop(); }

  bool start()
  {
    RelayOptions relayOptions;
    relayOptions.host = "127.0.0.1";
    relayOptions.devicePort = 0;
    relayOptions.homePort = 0;
    relayOptions.secret = options_.secret;
    relayOptions.limits = options_.limits;
    relayOptions.pushQueueCapacity = options_.relayPushCapacity;
    relay = std::make_unique<TunnelRelay>(loop, std::move(relayOptions));
    if (!relay->start())
      return false;

    TcpListener::Params gatewayParams;
    gatewayParams.loop = &loop;
    gatewayParams.ip = "127.0.0.1";
    gatewayParams.port = 0;
    gatewayParams.onAccept = [this](int fd, const std::string& ip,
                                    uint16_t port) {
      acceptGateway(fd, ip, port);
    };
    gatewayListener = TcpListener::create(gatewayParams);
    if (!gatewayListener)
      return false;

    ClientOptions clientOptions;
    clientOptions.relayHost = "127.0.0.1";
    clientOptions.relayPort = relay->homePort();
    clientOptions.gatewayHost = "127.0.0.1";
    clientOptions.gatewayPort = gatewayListener->boundPort();
    clientOptions.secret = options_.secret;
    clientOptions.reconnectWaitMs = 50;
    clientOptions.limits = options_.limits;
    clientOptions.pushQueueCapacity = options_.clientPushCapacity;
    client = std::make_unique<TunnelClient>(loop, std::move(clientOptions));

    loopThread = std::thread([this] { loop.run(); });
    client->start();
    return waitFor([this] { return client->homeActive(); }, 5000);
  }

  void stop()
  {
    if (!loopThread.joinable())
      return;
    client->stop();
    relay->stop();
    loop.stop();
    loopThread.join();
    client.reset();
    relay.reset();
    gatewayListener.reset();
  }

  std::vector<std::shared_ptr<StubConn>> gatewayConnections()
  {
    std::lock_guard<std::mutex> lock(gatewayMutex_);
    return gatewayConns_;
  }

  void unpauseGatewayReads()
  {
    std::vector<TcpPeer::Ptr> peers;
    {
      std::lock_guard<std::mutex> lock(gatewayMutex_);
      for (const auto& conn : gatewayConns_)
        peers.push_back(conn->peer);
    }
    loop.post([peers] {
      for (const auto& peer : peers)
        peer->setReadPaused(false);
    });
  }

  HarnessOptions options_;
  PollLoop loop;
  std::thread loopThread;
  std::unique_ptr<TunnelRelay> relay;
  std::unique_ptr<TunnelClient> client;
  std::unique_ptr<TcpListener> gatewayListener;

private:
  std::mutex gatewayMutex_;
  std::vector<std::shared_ptr<StubConn>> gatewayConns_;

  void acceptGateway(int fd, const std::string& ip, uint16_t port)
  {
    if (options_.gatewayRcvBuf > 0) {
      int size = options_.gatewayRcvBuf;
      ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    }
    auto conn = std::make_shared<StubConn>();
    TcpPeer::Params params;
    params.loop = &loop;
    params.fd = fd;
    params.ip = ip;
    params.port = port;
    TcpPeer::Callbacks callbacks;
    callbacks.onRead = [this, conn](TcpPeer& peer, const char* data,
                                    size_t size) {
      conn->received.append(data, size);
      if (options_.echoGateway)
        peer.send(data, size);
      if (options_.slowGatewayReadMs > 0) {
        peer.setReadPaused(true);
        const int slowMs = options_.slowGatewayReadMs;
        loop.runAfter(slowMs, [conn] { conn->peer->setReadPaused(false); });
      }
    };
    callbacks.onEof = [conn](TcpPeer&) { conn->eof = true; };
    params.callbacks = std::move(callbacks);
    conn->peer = TcpPeer::adopt(params);
    // adopt() does not fire onConnected (that is for outbound connects),
    // so the stub posture is applied right here.
    if (!options_.gatewayReply.empty())
      conn->peer->send(options_.gatewayReply);
    std::lock_guard<std::mutex> lock(gatewayMutex_);
    gatewayConns_.push_back(conn);
  }
};
} // namespace tunnel::test
