#pragma once

#include "event-query.hxx"
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/event/event-schema.hxx>
#include <shared/schemas/person-event/person-event-schema.hxx>
#include <vector>

class EventRepository
{
public:
  EventRepository() = default;
  ~EventRepository() = default;

  drogon::Task<std::optional<EventSchema>> findById(int64_t id) const;
  drogon::Task<std::vector<EventSchema>> findRecent(int64_t limit) const;
  drogon::Task<EventSchema> create(const EventCreateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;

  drogon::Task<void> linkPerson(const EventLinkPersonInput& input) const;
  drogon::Task<std::vector<PersonEventSchema>>
  findPersons(int64_t eventId) const;
};
