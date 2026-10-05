#pragma once

#include <feature/media/media-session-registry.hxx>
#include <nats/nats-bus.hxx>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class SessionRevocationConsumer
{
public:
  struct Dependencies
  {
    std::shared_ptr<NatsBus> bus;
    MediaSessionRegistry* sessions{nullptr};
    std::function<void(const MediaSessionKey&)> onRevoked;
  };

  struct Config
  {
    std::string stream;
    std::string subject;
    std::string durable;
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
  [[nodiscard]] bool subscribe();
  void scheduleSubscribeRetry();
  void handle(const NatsBus::DurableMessage& message,
              const NatsBus::DurableSettlement& settlement);

  Dependencies dependencies_;
  Config config_;
  std::optional<uint64_t> subscription_;
  std::optional<uint64_t> retryTimer_;
  std::atomic<int64_t> inFlight_{0};
};
