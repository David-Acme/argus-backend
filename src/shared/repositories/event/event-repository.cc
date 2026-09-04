#include "event-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <vector>

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

drogon::Task<std::vector<Json::Value>>
EventRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::readOnlyClient();
  const auto [query, args] =
      sync_query::buildSyncQuery(filter, SYNC_FIND, SYNC_FIND_FROM,
                                 SYNC_FIND_ALL, SYNC_FIND_AFTER,
                                 SYNC_FIND_AFTER_FROM);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(EventSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
EventRepository::findDeleted(const SyncFilter& filter) const
{
  auto client = DbService::readOnlyClient();
  const auto [query, args] = sync_query::buildSyncQuery(
      filter, SYNC_FIND_DELETED, SYNC_FIND_DELETED_FROM,
      SYNC_FIND_DELETED_ALL, SYNC_FIND_DELETED_AFTER,
      SYNC_FIND_DELETED_AFTER_FROM);
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(EventSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>> EventRepository::findLast(const SyncFilter&) const
{
  auto client = DbService::readOnlyClient();
  const auto result = co_await client->execSqlCoro(SYNC_FIND_LAST.data());
  if (result.empty())
    co_return std::nullopt;
  co_return EventSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>> EventRepository::findLastDeleted(const SyncFilter&) const
{
  auto client = DbService::readOnlyClient();
  const auto result =
      co_await client->execSqlCoro(SYNC_FIND_LAST_DELETED.data());
  if (result.empty())
    co_return std::nullopt;
  co_return EventSchema(result.front()).toJson();
}
