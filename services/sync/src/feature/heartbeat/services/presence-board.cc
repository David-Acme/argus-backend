#include "presence-board.hxx"

#include <feature/heartbeat/services/heartbeat-policy.hxx>

#include <algorithm>
#include <unordered_set>

namespace
{
PresenceEntry normalizedOf(const PresenceEntry& entry)
{
  return {.userId = entry.userId,
          .overall = heartbeat::normalizePresence(entry.overall),
          .since = entry.since};
}

bool sameState(const PresenceEntry& left, const PresenceEntry& right)
{
  return left.overall == right.overall && left.since == right.since;
}
}

bool PresenceBoard::apply(const PresenceEntry& entry)
{
  if (entry.userId <= 0)
    return false;
  PresenceEntry normalized = normalizedOf(entry);
  std::scoped_lock lock(mutex_);
  const uint64_t seq = ++sequence_;
  const auto found = entries_.find(entry.userId);
  if (found != entries_.end() && sameState(found->second.entry, normalized)) {
    found->second.appliedSeq = seq;
    return false;
  }
  entries_.insert_or_assign(entry.userId,
                            Slot{.entry = std::move(normalized), .appliedSeq = seq});
  return true;
}

uint64_t PresenceBoard::mark() const
{
  std::scoped_lock lock(mutex_);
  return sequence_;
}

std::vector<int64_t> PresenceBoard::fill(const PresenceFill& input)
{
  std::vector<int64_t> changed;
  std::unordered_set<int64_t> listed;
  listed.reserve(input.entries.size());
  std::scoped_lock lock(mutex_);
  for (const auto& entry : input.entries) {
    if (entry.userId <= 0)
      continue;
    listed.insert(entry.userId);
    PresenceEntry normalized = normalizedOf(entry);
    const auto found = entries_.find(entry.userId);
    if (found == entries_.end()) {
      entries_.emplace(entry.userId,
                       Slot{.entry = std::move(normalized), .appliedSeq = 0});
      changed.push_back(entry.userId);
      continue;
    }
    if (found->second.appliedSeq > input.readMark ||
        sameState(found->second.entry, normalized))
      continue;
    found->second.entry = std::move(normalized);
    changed.push_back(entry.userId);
  }
  std::erase_if(entries_, [&input, &listed, &changed](const auto& item) {
    if (listed.contains(item.first) || item.second.appliedSeq > input.readMark)
      return false;
    changed.push_back(item.first);
    return true;
  });
  std::ranges::sort(changed);
  return changed;
}

PresenceEntry PresenceBoard::of(int64_t userId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = entries_.find(userId);
  if (found == entries_.end())
    return {.userId = userId,
            .overall = std::string(heartbeat::kPresenceUnknown),
            .since = 0};
  return found->second.entry;
}

std::vector<int64_t> PresenceBoard::armedUsers() const
{
  std::vector<int64_t> users;
  std::scoped_lock lock(mutex_);
  for (const auto& [userId, slot] : entries_) {
    if (heartbeat::armed(slot.entry.overall))
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
