#include "presence-board.hxx"

#include <feature/heartbeat/services/heartbeat-policy.hxx>

#include <algorithm>

bool PresenceBoard::apply(const PresenceEntry& entry)
{
  if (entry.userId <= 0)
    return false;
  PresenceEntry normalized{.userId = entry.userId,
                           .overall = heartbeat::normalizePresence(entry.overall),
                           .since = entry.since};
  std::scoped_lock lock(mutex_);
  const auto found = entries_.find(entry.userId);
  if (found != entries_.end() && found->second.overall == normalized.overall &&
      found->second.since == normalized.since)
    return false;
  entries_.insert_or_assign(entry.userId, std::move(normalized));
  return true;
}

void PresenceBoard::fill(const std::vector<PresenceEntry>& entries)
{
  for (const auto& entry : entries)
    apply(entry);
}

PresenceEntry PresenceBoard::of(int64_t userId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = entries_.find(userId);
  if (found == entries_.end())
    return {.userId = userId,
            .overall = std::string(heartbeat::kPresenceUnknown),
            .since = 0};
  return found->second;
}

std::vector<int64_t> PresenceBoard::armedUsers() const
{
  std::vector<int64_t> users;
  std::scoped_lock lock(mutex_);
  for (const auto& [userId, entry] : entries_) {
    if (heartbeat::armed(entry.overall))
      users.push_back(userId);
  }
  std::ranges::sort(users);
  return users;
}

void PresenceBoard::markGuardSeen(int64_t at)
{
  int64_t previous = guardSeenAt_.load(std::memory_order_relaxed);
  while (previous < at &&
         !guardSeenAt_.compare_exchange_weak(previous, at,
                                             std::memory_order_relaxed)) {
  }
}

int64_t PresenceBoard::guardSeenAt() const
{
  return guardSeenAt_.load(std::memory_order_relaxed);
}
