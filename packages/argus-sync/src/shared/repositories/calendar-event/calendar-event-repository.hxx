#pragma once

#include "calendar-event-query.hxx"

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/contracts/syncable.hxx>
#include <shared/schemas/calendar-event/calendar-event-schema.hxx>
#include <vector>

struct CalendarEventRangeInput
{
  int64_t ownerId{0};
  int64_t from{0};
  int64_t to{0};
};

class CalendarEventRepository : public Syncable
{
public:
  CalendarEventRepository() = default;
  ~CalendarEventRepository() override = default;

  drogon::Task<std::optional<CalendarEventSchema>> findById(int64_t id) const;
  drogon::Task<std::vector<CalendarEventSchema>>
  findByOwnerRange(const CalendarEventRangeInput& input) const;
  drogon::Task<CalendarEventSchema>
  create(const CalendarEventCreateInput& input) const;
  drogon::Task<CalendarEventSchema>
  update(int64_t id, const CalendarEventUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
