#pragma once

#include <drogon/utils/coroutine.h>
#include <auth/jwt-filter.hxx>
#include <json/value.h>
#include <optional>
#include <sync/sync-filter.hxx>
#include <vector>

class NotificationSyncSource
{
public:
  virtual ~NotificationSyncSource() = default;

  [[nodiscard]] virtual drogon::Task<std::vector<Json::Value>>
  find(const JwtContext& ctx, const SyncFilter& filter) const = 0;

  [[nodiscard]] virtual drogon::Task<std::optional<Json::Value>>
  findLast(const JwtContext& ctx) const = 0;
};
