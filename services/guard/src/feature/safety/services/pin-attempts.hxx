#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>

struct PinAttemptAt
{
  int64_t userId{0};
  int64_t now{0};
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
  void fail(const PinAttemptAt& at);
  void clear(int64_t userId);

private:
  void prune(std::deque<int64_t>& failures, int64_t now) const;

  Limits limits_;
  mutable std::mutex mutex_;
  mutable std::unordered_map<int64_t, std::deque<int64_t>> failures_;
};
