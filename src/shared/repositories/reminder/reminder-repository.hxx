#pragma once

#include "reminder-query.hxx"

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/contracts/syncable.hxx>
#include <shared/schemas/reminder/reminder-schema.hxx>
#include <vector>

class ReminderRepository : public Syncable
{
public:
  ReminderRepository() = default;
  ~ReminderRepository() override = default;

  drogon::Task<std::optional<ReminderSchema>> findById(int64_t id) const;
  drogon::Task<std::vector<ReminderSchema>>
  findByTargetUser(int64_t targetUserId) const;
  drogon::Task<ReminderSchema> create(const ReminderCreateInput& input) const;
  drogon::Task<ReminderSchema> update(int64_t id,
                                      const ReminderUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>> findLast() const override;
  drogon::Task<std::optional<Json::Value>> findLastDeleted() const override;
};
