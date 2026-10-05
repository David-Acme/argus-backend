#pragma once

#include <auth/user-directory.hxx>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

struct CachedUserDirectoryConfig
{
  std::chrono::milliseconds ttl{10000};
  std::function<std::chrono::steady_clock::time_point()> clock;
};

class CachedUserDirectory final : public IUserDirectory
{
public:
  CachedUserDirectory(std::shared_ptr<const IUserDirectory> inner,
                      CachedUserDirectoryConfig config);

  drogon::Task<DirectoryLookup> lookup(int64_t userId) const override;

  void forget(int64_t userId) const;

private:
  struct Cached
  {
    DirectoryLookup lookup;
    std::chrono::steady_clock::time_point storedAt;
  };

  [[nodiscard]] std::chrono::steady_clock::time_point now() const;

  const std::shared_ptr<const IUserDirectory> inner_;
  const CachedUserDirectoryConfig config_;
  mutable std::mutex mutex_;
  mutable std::unordered_map<int64_t, Cached> entries_;
  mutable uint64_t epoch_{0};
};
