#include "cached-user-directory.hxx"

#include <utility>

CachedUserDirectory::CachedUserDirectory(
    std::shared_ptr<const IUserDirectory> inner,
    CachedUserDirectoryConfig config)
    : inner_(std::move(inner)), config_(std::move(config))
{
}

std::chrono::steady_clock::time_point CachedUserDirectory::now() const
{
  return config_.clock ? config_.clock() : std::chrono::steady_clock::now();
}

drogon::Task<DirectoryLookup> CachedUserDirectory::lookup(int64_t userId) const
{
  if (!inner_)
    co_return DirectoryLookup{};

  uint64_t epoch = 0;
  {
    std::scoped_lock lock(mutex_);
    const auto found = entries_.find(userId);
    if (found != entries_.end()) {
      if (now() - found->second.storedAt < config_.ttl)
        co_return found->second.lookup;
      entries_.erase(found);
    }
    epoch = epoch_;
  }

  DirectoryLookup fresh = co_await inner_->lookup(userId);
  if (fresh.status == DirectoryLookupStatus::Unavailable)
    co_return fresh;

  std::scoped_lock lock(mutex_);
  if (epoch == epoch_)
    entries_.insert_or_assign(userId, Cached{.lookup = fresh, .storedAt = now()});
  co_return fresh;
}

void CachedUserDirectory::forget(int64_t userId) const
{
  std::scoped_lock lock(mutex_);
  entries_.erase(userId);
  ++epoch_;
}
