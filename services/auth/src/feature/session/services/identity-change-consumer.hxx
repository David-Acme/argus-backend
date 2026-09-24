#pragma once

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <nats/nats-bus.hxx>
#include <optional>
#include <string>

class SessionService;

class IdentityChangeConsumer
{
public:
  struct Config
  {
    std::string stream;
    std::string durable;
    std::string subject;
    int maxDeliver{10};
  };

  struct Dependencies
  {
    NatsBus* bus{nullptr};
    SessionService* sessions{nullptr};
  };

  IdentityChangeConsumer(Dependencies dependencies, Config config);
  ~IdentityChangeConsumer();

  IdentityChangeConsumer(const IdentityChangeConsumer&) = delete;
  IdentityChangeConsumer& operator=(const IdentityChangeConsumer&) = delete;

  void start();
  void stop();

  void requestStop();

  [[nodiscard]] bool drained() const;

private:
  [[nodiscard]] drogon::Task<void> handle(const std::string& body);
  [[nodiscard]] bool subscribe();
  void scheduleSubscribeRetry();

  Dependencies dependencies_;
  Config config_;
  std::optional<uint64_t> subscription_;
  std::optional<uint64_t> retryTimer_;
  std::atomic<int64_t> inFlight_{0};
};
