#include "rate-limiter.hxx"

#include <utility>

RateLimiter::RateLimiter(IdentityRateLimitConfig config) : config_(config) {}

void RateLimiter::prune(std::chrono::steady_clock::time_point now)
{
  const auto windowStart = now - std::chrono::seconds(config_.windowSeconds);
  std::erase_if(entries_, [&](const auto& entry) {
    const bool idle = entry.second.hits.empty() || entry.second.hits.back() < windowStart;
    return idle && !(now < entry.second.lockedUntil);
  });
}

bool RateLimiter::admit(const RateLimiterKey& input)
{
  if (!config_.enabled)
    return true;
  const std::scoped_lock lock(mutex_);
  auto it = entries_.find(input.key);
  if (it == entries_.end()) {
    if (entries_.size() >= kMaxTrackedKeys)
      prune(input.now);
    if (entries_.size() >= kMaxTrackedKeys)
      return false;
    it = entries_.emplace(input.key, Entry{}).first;
  }
  Entry& entry = it->second;
  if (input.now < entry.lockedUntil)
    return false;
  const auto windowStart = input.now - std::chrono::seconds(config_.windowSeconds);
  while (!entry.hits.empty() && entry.hits.front() < windowStart)
    entry.hits.pop_front();
  if (std::cmp_greater_equal(entry.hits.size(), config_.maxRequests))
    return false;
  entry.hits.push_back(input.now);
  return true;
}

void RateLimiter::recordFailure(const RateLimiterKey& input)
{
  if (!config_.enabled)
    return;
  const std::scoped_lock lock(mutex_);
  const auto it = entries_.find(input.key);
  if (it == entries_.end())
    return;
  if (++it->second.consecutiveFailures < config_.lockoutThreshold)
    return;
  it->second.consecutiveFailures = 0;
  it->second.lockedUntil = input.now + std::chrono::seconds(config_.lockoutSeconds);
}

void RateLimiter::recordSuccess(const std::string& key)
{
  const std::scoped_lock lock(mutex_);
  if (const auto it = entries_.find(key); it != entries_.end())
    it->second.consecutiveFailures = 0;
}

std::size_t RateLimiter::tracked() const
{
  const std::scoped_lock lock(mutex_);
  return entries_.size();
}
