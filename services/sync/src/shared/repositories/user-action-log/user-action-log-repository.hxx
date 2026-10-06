#pragma once
#include "user-action-log-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/user-action-log/user-action-log-schema.hxx>
#include <string>
#include <sync/syncable.hxx>
#include <vector>

class UserActionLogRepository : public Syncable
{
public:
  UserActionLogRepository() = default;

  [[nodiscard]] bool migrateLegacySchema() const;
  [[nodiscard]] bool backfillModules() const;
  [[nodiscard]] static std::string activityStatement(const ActivityListInput& input);

  drogon::Task<std::optional<UserActionLogSchema>>
  create(const UserActionLogCreateInput& input) const;

  drogon::Task<std::vector<UserActionLogSchema>>
  list(const ActivityListInput& input) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;

  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;

  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;

  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
