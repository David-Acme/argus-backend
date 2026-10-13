#pragma once

#include <feature/safety/repositories/pin-attempt/pin-attempt-repository.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <mutex>
#include <unordered_set>

struct PinAttemptAt
{
  int64_t userId{0};
  int64_t now{0};
};

struct PinReservation
{
  bool admitted{false};
  bool locked{false};
};

struct PinSettlement
{
  int64_t userId{0};
  bool success{false};
};

class PinAttempts
{
public:
  struct Limits
  {
    int maxFailures{5};
    int64_t windowSeconds{900};
  };

  explicit PinAttempts(Limits limits);

  [[nodiscard]] drogon::Task<bool> locked(PinAttemptAt at) const;
  [[nodiscard]] drogon::Task<PinReservation> reserve(PinAttemptAt at);
  drogon::Task<void> settle(PinSettlement settlement);
  drogon::Task<void> clear(int64_t userId);
  [[nodiscard]] drogon::Task<int64_t> purgeExpired(int64_t now) const;
  void release(int64_t userId);

private:
  Limits limits_;
  PinAttemptRepository repository_;
  std::mutex mutex_;
  std::unordered_set<int64_t> inFlight_;
};
