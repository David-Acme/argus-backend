#include "pin-attempts.hxx"

#include <utility>

PinAttempts::PinAttempts(Limits limits) : limits_(limits) {}

void PinAttempts::prune(std::deque<int64_t>& failures, int64_t now) const
{
  while (!failures.empty() && now - failures.front() >= limits_.windowSeconds)
    failures.pop_front();
}

bool PinAttempts::locked(const PinAttemptAt& at) const
{
  const int64_t userId = at.userId;
  const int64_t now = at.now;
  std::scoped_lock lock(mutex_);
  const auto found = failures_.find(userId);
  if (found == failures_.end())
    return false;
  prune(found->second, now);
  return std::cmp_greater_equal(found->second.size(), limits_.maxFailures);
}

void PinAttempts::fail(const PinAttemptAt& at)
{
  std::scoped_lock lock(mutex_);
  auto& failures = failures_[at.userId];
  prune(failures, at.now);
  failures.push_back(at.now);
}

void PinAttempts::clear(int64_t userId)
{
  std::scoped_lock lock(mutex_);
  failures_.erase(userId);
}
