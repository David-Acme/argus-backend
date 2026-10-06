#pragma once

#include "reminder-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/schemas/reminder/reminder-schema.hxx>
#include <sync/syncable.hxx>
#include <vector>

class ReminderRepository : public Syncable
{
public:
  ReminderRepository() = default;
  ~ReminderRepository() override = default;

  drogon::Task<std::optional<ReminderSchema>>
  findOwned(const ReminderOwnedInput& input) const;
  drogon::Task<std::vector<ReminderSchema>>
  findByTargetUser(const ReminderListInput& input) const;
  drogon::Task<ReminderSchema> create(const ReminderCreateInput& input) const;
  drogon::Task<ReminderSchema> update(const ReminderUpdateInput& input) const;
  drogon::Task<bool> remove(const ReminderOwnedInput& input) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
