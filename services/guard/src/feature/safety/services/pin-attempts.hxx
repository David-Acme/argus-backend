#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
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

  [[nodiscard]] bool locked(const PinAttemptAt& at) const;
  [[nodiscard]] PinReservation reserve(const PinAttemptAt& at);
  void settle(const PinSettlement& settlement);
  void clear(int64_t userId);

private:
  void prune(std::deque<int64_t>& failures, int64_t now) const;
  [[nodiscard]] bool lockedLocked(int64_t userId, int64_t now) const;

  Limits limits_;
  mutable std::mutex mutex_;
  mutable std::unordered_map<int64_t, std::deque<int64_t>> failures_;
  std::unordered_set<int64_t> inFlight_;
};
