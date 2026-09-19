#include "refresh-rate-limiter.hxx"

#include <shared/services/config-service/config-service.hxx>

namespace
{
constexpr int kDefaultWindowSeconds = 60;
constexpr int kDefaultMaxRequests = 10;
constexpr int kDefaultLockoutThreshold = 5;
constexpr int kDefaultLockoutSeconds = 300;
// Bound on tracked keys so rotated fingerprints cannot grow the map forever.
constexpr size_t kMaxTrackedKeys = 4096;

int positiveOr(int value, int fallback)
{
  return value > 0 ? value : fallback;
}
} // namespace

RateLimitConfig RateLimitConfig::resolve()
{
  RateLimitConfig config;
  config.enabled = ConfigService::getBool("rate_limit.enabled");
  config.windowSeconds = positiveOr(
      ConfigService::getInt("rate_limit.window_seconds"),
      kDefaultWindowSeconds);
  config.maxRequests =
      positiveOr(ConfigService::getInt("rate_limit.max_requests"),
                 kDefaultMaxRequests);
  config.lockoutThreshold = positiveOr(
      ConfigService::getInt("rate_limit.lockout_threshold"),
      kDefaultLockoutThreshold);
  config.lockoutSeconds = positiveOr(
      ConfigService::getInt("rate_limit.lockout_seconds"),
      kDefaultLockoutSeconds);
  return config;
}

RefreshRateLimiter::RefreshRateLimiter(RateLimitConfig config)
    : config_(config)
{
}

bool RefreshRateLimiter::admit(const std::string& key,
                               std::chrono::steady_clock::time_point now)
{
  if (!config_.enabled)
    return true;

  const std::lock_guard<std::mutex> lock(mutex_);
  const auto windowStart = now - std::chrono::seconds(config_.windowSeconds);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    if (entries_.size() >= kMaxTrackedKeys) {
      pruneExpired(now);
      if (entries_.size() >= kMaxTrackedKeys)
        return false;
    }
    it = entries_.emplace(key, Entry{}).first;
  }
  if (now < it->second.lockedUntil)
    return false;
  while (!it->second.hits.empty() && it->second.hits.front() < windowStart)
    it->second.hits.pop_front();
  if (static_cast<int>(it->second.hits.size()) >= config_.maxRequests)
    return false;
  it->second.hits.push_back(now);
  return true;
}

bool RefreshRateLimiter::recordResult(const RecordResultInput& input)
{
  const std::string& key = input.key;
  const bool success = input.success;
  const std::chrono::steady_clock::time_point now = input.now;

  if (!config_.enabled)
    return false;

  const std::lock_guard<std::mutex> lock(mutex_);
  const auto it = entries_.find(key);
  if (it == entries_.end())
    return false;
  if (now < it->second.lockedUntil)
    return false;
  if (success) {
    it->second.consecutiveFailures = 0;
    return false;
  }
  ++it->second.consecutiveFailures;
  if (it->second.consecutiveFailures >= config_.lockoutThreshold) {
    it->second.lockedUntil = now + std::chrono::seconds(config_.lockoutSeconds);
    return true;
  }
  return false;
}

void RefreshRateLimiter::pruneExpired(std::chrono::steady_clock::time_point now)
{
  const auto windowStart = now - std::chrono::seconds(config_.windowSeconds);
  for (auto it = entries_.begin(); it != entries_.end();) {
    const bool hitsExpired = it->second.hits.empty()
                             || it->second.hits.front() < windowStart;
    const bool lockoutExpired = !(now < it->second.lockedUntil);
    if (hitsExpired && lockoutExpired)
      it = entries_.erase(it);
    else
      ++it;
  }
}
