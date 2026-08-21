#pragma once

#include "event-query.hxx"

#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <shared/contracts/syncable.hxx>
#include <optional>
#include <shared/schemas/event/event-schema.hxx>
#include <shared/schemas/person-event/person-event-schema.hxx>
#include <vector>

class EventRepository : public Syncable
{
public:
  EventRepository() = default;
  ~EventRepository() override = default;

  drogon::Task<std::optional<EventSchema>> findById(int64_t id) const;
  drogon::Task<std::vector<EventSchema>> findRecent(int64_t limit) const;
  drogon::Task<EventSchema> create(const EventCreateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;

  drogon::Task<void> linkPerson(const EventLinkPersonInput& input) const;
  drogon::Task<std::vector<PersonEventSchema>>
  findPersons(int64_t eventId) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
