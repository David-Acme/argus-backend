#pragma once

#include "calendar-event-share-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <sync/syncable.hxx>
#include <shared/schemas/calendar-event-share/calendar-event-share-schema.hxx>
#include <vector>

class CalendarEventShareRepository : public Syncable
{
public:
  CalendarEventShareRepository() = default;
  ~CalendarEventShareRepository() override = default;

  drogon::Task<std::optional<CalendarEventShareSchema>>
  findById(int64_t id, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::vector<CalendarEventShareSchema>> findByParent(int64_t parentId) const;
  drogon::Task<std::optional<ShareAccess>>
  findAccess(const CalendarEventShareLookupInput& input) const;
  drogon::Task<std::vector<int64_t>>
  memberIds(int64_t parentId, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::optional<CalendarEventShareSchema>>
  findExisting(const CalendarEventShareLookupInput& input) const;
  drogon::Task<CalendarEventShareSchema>
  create(const CalendarEventShareCreateInput& input) const;
  drogon::Task<CalendarEventShareSchema>
  updateAccess(const CalendarEventShareUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id,
                            drogon::orm::DbClient* client = nullptr) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
