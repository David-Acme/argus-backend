#pragma once

#include <feature/media/media-session-registry.hxx>
#include <nats/nats-bus.hxx>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

class SessionRevocationConsumer
{
public:
  struct Dependencies
  {
    std::shared_ptr<NatsBus> bus;
    MediaSessionRegistry* sessions{nullptr};
    std::function<void(const MediaSessionKey&)> onRevoked;
    std::function<void(int64_t)> onUserChanged{};
  };

  struct Feed
  {
    std::string stream;
    std::string subject;
    std::string durable;
  };

  struct Config
  {
    Feed sessions;
    Feed users;
    int maxDeliver{10};
  };

  [[nodiscard]] static Config defaults();

  SessionRevocationConsumer(Dependencies dependencies, Config config);
  ~SessionRevocationConsumer();

  SessionRevocationConsumer(const SessionRevocationConsumer&) = delete;
  SessionRevocationConsumer& operator=(const SessionRevocationConsumer&) = delete;

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  [[nodiscard]] bool subscribe(const Feed& feed);
  [[nodiscard]] bool subscribePending();
  void connect(const std::stop_token& stop);
  void handle(const NatsBus::DurableMessage& message,
              const NatsBus::DurableSettlement& settlement);
  void handleSession(const std::string& body, const NatsBus::DurableSettlement& settlement);
  void handleUser(const std::string& body, const NatsBus::DurableSettlement& settlement);

  Dependencies dependencies_;
  Config config_;
  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::optional<uint64_t> sessionSubscription_;
  std::optional<uint64_t> userSubscription_;
  std::atomic<int64_t> inFlight_{0};
  std::atomic<bool> connecting_{false};
  std::jthread connector_;
};
