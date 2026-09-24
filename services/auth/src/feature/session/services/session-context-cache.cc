#include "session-context-cache.hxx"

#include <chrono>
#include <identity/identity-client.hxx>
#include <runtime/blocking-task.hxx>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

SessionContextCache::SessionContextCache(const IdentityClient* identity,
                                         Config config)
    : identity_(identity), config_(config)
{
}

std::optional<UserContext> SessionContextCache::hit(int64_t userId) const
{
  const std::scoped_lock lock(mutex_);
  const auto entry = entries_.find(userId);
  if (entry == entries_.end())
    return std::nullopt;
  if (entry->second.expiresAtMs <= nowMs()) {
    entries_.erase(entry);
    return std::nullopt;
  }
  return entry->second.context;
}

int64_t SessionContextCache::generation() const
{
  const std::scoped_lock lock(mutex_);
  return generation_;
}

drogon::Task<std::optional<UserContext>>
SessionContextCache::resolve(int64_t userId) const
{
  if (userId <= 0)
    co_return std::nullopt;
  if (const auto cached = hit(userId); cached.has_value())
    co_return cached;
  if (identity_ == nullptr)
    co_return std::nullopt;

  const int64_t generationAtFetch = generation();
  const auto answer = co_await BlockingTask<
      std::optional<argus::identity::v1::GetUserResponse>>(
      [this, userId]() { return identity_->getUser(userId); });
  if (!answer.has_value() || !answer->has_user())
    co_return std::nullopt;

  const auto& user = answer->user();
  const UserContext context{
      .userId = user.user_id(),
      .name = user.name(),
      .lastName = user.last_name(),
      .lang = user.lang(),
      .role = user.role(),
      .isActive = user.is_active(),
  };
  if (config_.ttlSeconds > 0) {
    const int64_t expiresAtMs = nowMs() + config_.ttlSeconds * 1000;
    const std::scoped_lock lock(mutex_);
    if (generation_ == generationAtFetch)
      entries_.insert_or_assign(userId,
                                Entry{.context = context,
                                      .expiresAtMs = expiresAtMs});
  }
  co_return context;
}

void SessionContextCache::forget(int64_t userId)
{
  const std::scoped_lock lock(mutex_);
  entries_.erase(userId);
  ++generation_;
}
