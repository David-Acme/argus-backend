#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/session/services/user-context.hxx>
#include <mutex>
#include <optional>
#include <unordered_map>

class IdentityClient;

class SessionContextCache
{
public:
  struct Config
  {
    int64_t ttlSeconds{30};
  };

  SessionContextCache(const IdentityClient* identity, Config config);

  [[nodiscard]] drogon::Task<std::optional<UserContext>>
  resolve(int64_t userId) const;

  void forget(int64_t userId);

private:
  struct Entry
  {
    UserContext context;
    int64_t expiresAtMs{0};
  };

  [[nodiscard]] std::optional<UserContext> hit(int64_t userId) const;
  [[nodiscard]] int64_t generation() const;

  const IdentityClient* identity_;
  Config config_;
  mutable std::mutex mutex_;
  mutable std::unordered_map<int64_t, Entry> entries_;
  mutable int64_t generation_{0};
};
