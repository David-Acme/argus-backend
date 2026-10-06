#include "pending-turn.hxx"

#include <utility>

namespace turn
{

void PendingTurns::put(int64_t userId, Pending pending)
{
  pending.at = std::chrono::steady_clock::now();
  const std::scoped_lock lock(mutex_);
  pendings_[userId] = std::move(pending);
}

std::optional<Pending> PendingTurns::peek(int64_t userId) const
{
  const std::scoped_lock lock(mutex_);
  const auto found = pendings_.find(userId);
  if (found == pendings_.end())
    return std::nullopt;
  if (std::chrono::steady_clock::now() - found->second.at > kLife) {
    pendings_.erase(found);
    return std::nullopt;
  }
  return found->second;
}

void PendingTurns::forget(int64_t userId)
{
  const std::scoped_lock lock(mutex_);
  pendings_.erase(userId);
}

}
