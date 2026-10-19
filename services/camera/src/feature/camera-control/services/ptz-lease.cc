#include "ptz-lease.hxx"

PtzLease& PtzLease::instance()
{
  static PtzLease lease;
  return lease;
}

void PtzLease::configure(PtzLeaseConfig config)
{
  std::scoped_lock lock(mutex_);
  config_ = config;
}

void PtzLease::vacate(Entry& entry, int64_t atMs) const
{
  if (entry.holder == PtzHolder::Person || entry.holder == PtzHolder::Viewer)
    entry.parkUntilMs = atMs + config_.parkMs;
  entry.holder = PtzHolder::None;
  entry.userId = 0;
  entry.sinceMs = 0;
  entry.lastActiveMs = 0;
}

void PtzLease::expire(Entry& entry, int64_t atMs) const
{
  if (entry.holder != PtzHolder::Person && entry.holder != PtzHolder::Viewer)
    return;
  if (atMs - entry.lastActiveMs < config_.idleReleaseMs)
    return;
  const int64_t releasedAt = entry.lastActiveMs + config_.idleReleaseMs;
  vacate(entry, releasedAt);
}

PtzClaimResult PtzLease::claim(const PtzClaim& claim)
{
  std::scoped_lock lock(mutex_);
  Entry& entry = entries_[claim.cameraId];
  expire(entry, claim.atMs);
  const bool sameHolder = entry.holder == claim.holder && entry.userId == claim.userId;
  if (sameHolder) {
    entry.lastActiveMs = claim.atMs;
    return PtzClaimResult::Granted;
  }
  if (claim.holder == PtzHolder::Argus &&
      (entry.holder != PtzHolder::None || claim.atMs < entry.parkUntilMs))
    return PtzClaimResult::Refused;
  if (claim.holder < entry.holder)
    return PtzClaimResult::Refused;
  const bool preempted = entry.holder != PtzHolder::None;
  entry.holder = claim.holder;
  entry.userId = claim.userId;
  entry.sinceMs = claim.atMs;
  entry.lastActiveMs = claim.atMs;
  return preempted ? PtzClaimResult::Preempted : PtzClaimResult::Granted;
}

void PtzLease::release(const PtzClaim& claim)
{
  std::scoped_lock lock(mutex_);
  const auto it = entries_.find(claim.cameraId);
  if (it == entries_.end())
    return;
  Entry& entry = it->second;
  if (entry.holder != claim.holder || entry.userId != claim.userId)
    return;
  vacate(entry, claim.atMs);
}

PtzLeaseState PtzLease::stateOf(const PtzInstant& instant)
{
  std::scoped_lock lock(mutex_);
  const auto it = entries_.find(instant.cameraId);
  if (it == entries_.end())
    return {};
  expire(it->second, instant.atMs);
  return {.holder = it->second.holder,
          .userId = it->second.userId,
          .sinceMs = it->second.sinceMs,
          .parkUntilMs = it->second.parkUntilMs};
}

bool PtzLease::holds(const PtzClaim& claim)
{
  std::scoped_lock lock(mutex_);
  const auto it = entries_.find(claim.cameraId);
  if (it == entries_.end())
    return false;
  expire(it->second, claim.atMs);
  return it->second.holder == claim.holder && it->second.userId == claim.userId;
}

void PtzLease::forget(int64_t cameraId)
{
  std::scoped_lock lock(mutex_);
  entries_.erase(cameraId);
}
