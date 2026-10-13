#include "pin-attempts.hxx"

#include <utility>

PinAttempts::PinAttempts(Limits limits) : limits_(limits) {}

drogon::Task<bool> PinAttempts::locked(PinAttemptAt at) const
{
  const int64_t failures =
      co_await repository_.failuresSince({.userId = at.userId, .since = at.now - limits_.windowSeconds});
  co_return std::cmp_greater_equal(failures, limits_.maxFailures);
}

drogon::Task<PinReservation> PinAttempts::reserve(PinAttemptAt at)
{
  bool admitted = false;
  {
    const std::scoped_lock lock(mutex_);
    admitted = inFlight_.insert(at.userId).second;
  }
  if (!admitted)
    co_return PinReservation{.admitted = false, .locked = false};
  try {
    const bool lockedOut = co_await locked(at);
    if (!lockedOut)
      co_await repository_.recordFailure({.userId = at.userId, .at = at.now});
    co_return PinReservation{.admitted = true, .locked = lockedOut};
  }
  catch (...) {
    release(at.userId);
    throw;
  }
}

drogon::Task<void> PinAttempts::settle(PinSettlement settlement)
{
  release(settlement.userId);
  if (settlement.success)
    co_await repository_.clear(settlement.userId);
}

drogon::Task<void> PinAttempts::clear(int64_t userId)
{
  co_await repository_.clear(userId);
}

drogon::Task<int64_t> PinAttempts::purgeExpired(int64_t now) const
{
  co_return co_await repository_.purgeBefore(now - limits_.windowSeconds);
}

void PinAttempts::release(int64_t userId)
{
  const std::scoped_lock lock(mutex_);
  inFlight_.erase(userId);
}
