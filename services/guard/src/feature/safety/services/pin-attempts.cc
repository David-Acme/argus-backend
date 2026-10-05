#include "pin-attempts.hxx"

#include <utility>

PinAttempts::PinAttempts(Limits limits) : limits_(limits) {}

void PinAttempts::prune(std::deque<int64_t>& failures, int64_t now) const
{
  while (!failures.empty() && now - failures.front() >= limits_.windowSeconds)
    failures.pop_front();
}

bool PinAttempts::lockedLocked(int64_t userId, int64_t now) const
{
  const auto found = failures_.find(userId);
  if (found == failures_.end())
    return false;
  prune(found->second, now);
  return std::cmp_greater_equal(found->second.size(), limits_.maxFailures);
}

bool PinAttempts::locked(const PinAttemptAt& at) const
{
  std::scoped_lock lock(mutex_);
  return lockedLocked(at.userId, at.now);
}

PinReservation PinAttempts::reserve(const PinAttemptAt& at)
{
  std::scoped_lock lock(mutex_);
  const bool lockedOut = lockedLocked(at.userId, at.now);
  if (!inFlight_.insert(at.userId).second)
    return {.admitted = false, .locked = lockedOut};
  if (!lockedOut)
    failures_[at.userId].push_back(at.now);
  return {.admitted = true, .locked = lockedOut};
}

void PinAttempts::settle(const PinSettlement& settlement)
{
  std::scoped_lock lock(mutex_);
  inFlight_.erase(settlement.userId);
  if (settlement.success)
    failures_.erase(settlement.userId);
}

void PinAttempts::clear(int64_t userId)
{
  std::scoped_lock lock(mutex_);
  failures_.erase(userId);
}
