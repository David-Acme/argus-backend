#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

struct RateLimitConfig
{
  bool enabled{false};
  int windowSeconds{60};
  int maxRequests{10};
  int lockoutThreshold{5};
  int lockoutSeconds{300};

  static RateLimitConfig resolve();
};

class RefreshRateLimiter
{
public:
  explicit RefreshRateLimiter(RateLimitConfig config);

  bool enabled() const { return config_.enabled; }

  bool admit(const std::string& key,
             std::chrono::steady_clock::time_point now);

  struct RecordResultInput
  {
    const std::string& key;
    bool success{false};
    std::chrono::steady_clock::time_point now{};
  };
  bool recordResult(const RecordResultInput& input);

private:
  struct Entry
  {
    std::deque<std::chrono::steady_clock::time_point> hits;
    int consecutiveFailures{0};
    std::chrono::steady_clock::time_point lockedUntil{};
  };

  void pruneExpired(std::chrono::steady_clock::time_point now);

  RateLimitConfig config_;
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
};
