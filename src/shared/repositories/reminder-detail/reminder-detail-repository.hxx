#pragma once

#include "reminder-detail-query.hxx"

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/contracts/syncable.hxx>
#include <shared/schemas/reminder-detail/reminder-detail-schema.hxx>
#include <vector>

class ReminderDetailRepository : public Syncable
{
public:
  ReminderDetailRepository() = default;
  ~ReminderDetailRepository() override = default;

  drogon::Task<std::optional<ReminderDetailSchema>> findById(int64_t id) const;
  drogon::Task<std::vector<ReminderDetailSchema>>
  findByReminder(int64_t reminderId) const;
  drogon::Task<ReminderDetailSchema>
  create(const ReminderDetailCreateInput& input) const;
  drogon::Task<ReminderDetailSchema>
  update(int64_t id, const ReminderDetailUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>> findLast() const override;
  drogon::Task<std::optional<Json::Value>> findLastDeleted() const override;
};
