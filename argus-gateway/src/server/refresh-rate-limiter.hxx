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

  // Ruling CJ: resolves [rate_limit]; enabled=false (default) never rejects.
  static RateLimitConfig resolve();
};

// Ruling CJ: in-memory sliding-window counter plus consecutive-failure
// lockout. Single-instance state only: a restart clears every counter and
// lockout, and the tracked-key set is bounded.
class RefreshRateLimiter
{
public:
  explicit RefreshRateLimiter(RateLimitConfig config);

  bool enabled() const { return config_.enabled; }

  // Returns false when the request must be rejected with 429.
  bool admit(const std::string& key,
             std::chrono::steady_clock::time_point now);
  // Returns true when this failure just locked the key.
  bool recordResult(const std::string& key, bool success,
                    std::chrono::steady_clock::time_point now);

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
