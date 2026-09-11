#pragma once

#include <drogon/utils/coroutine.h>
#include <filter/jwt/jwt-filter.hxx>
#include <json/value.h>
#include <optional>
#include <shared/contracts/sync-filter.hxx>
#include <vector>

// User-scoped notification pull source (owner: argus-notification).
class NotificationSyncSource
{
public:
  virtual ~NotificationSyncSource() = default;

  virtual drogon::Task<std::vector<Json::Value>>
  find(const JwtContext& ctx, const SyncFilter& filter) const = 0;

  virtual drogon::Task<std::optional<Json::Value>>
  findLast(const JwtContext& ctx) const = 0;
};
