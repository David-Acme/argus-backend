#include "event-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace event_query;

drogon::Task<std::optional<EventSchema>>
EventRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_ID.data(), id);
  if (result.empty())
    co_return std::nullopt;
  co_return EventSchema(result.front());
}

drogon::Task<std::vector<EventSchema>>
EventRepository::findRecent(int64_t limit) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_RECENT.data(), limit);

  std::vector<EventSchema> data;
  for (const auto& row : result)
    data.push_back(EventSchema(row));
  co_return data;
}

drogon::Task<EventSchema>
EventRepository::create(const EventCreateInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(INSERT.data(), input.eventType,
                                   eventSeverityToString(input.severity),
                                   input.source, input.summary, input.details,
                                   input.occurredAt);

  EventSchema schema;
  schema.id = result.insertId();
  schema.eventType = input.eventType;
  schema.severity = input.severity;
  schema.source = input.source;
  schema.summary = input.summary;
  schema.details = input.details;
  schema.occurredAt = input.occurredAt;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<bool> EventRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(REMOVE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<void>
EventRepository::linkPerson(const EventLinkPersonInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(LINK_PERSON.data(), input.personId,
                               input.eventId, input.confidence);
}

drogon::Task<std::vector<PersonEventSchema>>
EventRepository::findPersons(int64_t eventId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_PERSONS_BY_EVENT.data(), eventId);

  std::vector<PersonEventSchema> data;
  for (const auto& row : result)
    data.push_back(PersonEventSchema(row));
  co_return data;
}
