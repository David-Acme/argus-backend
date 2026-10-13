#pragma once

#include "link-test-cert.hxx"

#include <link/client/link-client.hxx>
#include <link/protocol/frames.hxx>
#include <net/poll-loop.hxx>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tunnel::test
{
bool waitFor(const std::function<bool()>& predicate, int timeoutMs);

class EchoServer
{
public:
  EchoServer();
  ~EchoServer();
  EchoServer(const EchoServer&) = delete;
  EchoServer& operator=(const EchoServer&) = delete;

  [[nodiscard]] uint16_t port() const { return port_; }
  [[nodiscard]] size_t accepted() const { return accepted_.load(); }
  [[nodiscard]] size_t received() const { return received_.load(); }
  void stop();

private:
  void serve();

  int listenFd_{-1};
  uint16_t port_{0};
  std::jthread worker_;
  std::atomic<bool> stopped_{false};
  std::atomic<size_t> accepted_{0};
  std::atomic<size_t> received_{0};
};

struct FakeNodeOptions
{
  std::string certificatePath;
  std::string keyPath;
  uint32_t capabilities{link::capability::kPushIntents |
                        link::capability::kStreamCredit};
  bool sendHello{true};
  bool answerPings{true};
  bool acceptReconnects{false};
};

class FakeNode
{
public:
  struct State
  {
    bool handshaken{false};
    bool helloSeen{false};
    std::string peerCommonName;
    std::vector<link::WireError> violations;
    std::string lastError;
  };

  explicit FakeNode(FakeNodeOptions options);
  ~FakeNode();
  FakeNode(const FakeNode&) = delete;
  FakeNode& operator=(const FakeNode&) = delete;

  [[nodiscard]] uint16_t port() const { return port_; }
  [[nodiscard]] size_t connections() const { return connections_.load(); }
  bool sendFrame(std::string_view frame);
  bool sendHello();
  void closeConnection();
  std::optional<link::Frame> nextFrame(int timeoutMs);
  [[nodiscard]] State state() const;
  void stop();

private:
  void serve();
  void readLoop(SSL* ssl);
  void handleFrame(SSL* ssl, const link::Frame& frame);

  FakeNodeOptions options_;
  int listenFd_{-1};
  uint16_t port_{0};
  SSL* ssl_{nullptr};
  std::jthread worker_;
  std::atomic<bool> stopped_{false};
  std::atomic<size_t> connections_{0};
  mutable std::mutex mutex_;
  std::deque<link::Frame> frames_;
  State state_;
};

struct LinkFixtureOptions
{
  TestIdentity home;
  TestIdentity node;
  FakeNodeOptions nodeOptions;
  tunnel::link::LinkOptions link;
  uint16_t gatewayPort{0};
  std::string pinnedSubjectPublicKeyDer;
  tunnel::link::LinkObserver* observer{nullptr};
};

class LinkFixture
{
public:
  explicit LinkFixture(LinkFixtureOptions options);
  ~LinkFixture();
  LinkFixture(const LinkFixture&) = delete;
  LinkFixture& operator=(const LinkFixture&) = delete;

  tunnel::link::LinkClient& client() { return *client_; }
  FakeNode& node() { return node_; }
  EchoServer& echo() { return echo_; }
  [[nodiscard]] bool waitConnected(int timeoutMs);
  void drainFrames();
  [[nodiscard]] const std::optional<tunnel::link::Frame>& clientHello() const
  {
    return hello_;
  }
  tunnel::link::Frame awaitFrame(int timeoutMs);

private:
  LinkFixtureOptions options_;
  EchoServer echo_;
  FakeNode node_;
  PollLoop loop_;
  std::unique_ptr<tunnel::link::LinkClient> client_;
  std::optional<tunnel::link::Frame> hello_;
  std::thread loopThread_;
};
}
