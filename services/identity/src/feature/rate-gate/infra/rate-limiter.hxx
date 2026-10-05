#pragma once

#include <chrono>
#include <config/identity-config.hxx>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

struct RateLimiterKey
{
  std::string key;
  std::chrono::steady_clock::time_point now;
};

class RateLimiter
{
public:
  explicit RateLimiter(IdentityRateLimitConfig config);

  [[nodiscard]] bool admit(const RateLimiterKey& input);
  void recordFailure(const RateLimiterKey& input);
  void recordSuccess(const std::string& key);
  [[nodiscard]] std::size_t tracked() const;

  static constexpr std::size_t kMaxTrackedKeys = 4096;

private:
  struct Entry
  {
    std::deque<std::chrono::steady_clock::time_point> hits;
    int consecutiveFailures{0};
    std::chrono::steady_clock::time_point lockedUntil{};
  };

  void prune(std::chrono::steady_clock::time_point now);

  IdentityRateLimitConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
};
