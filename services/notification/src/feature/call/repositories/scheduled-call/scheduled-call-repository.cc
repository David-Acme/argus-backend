#include "scheduled-call-repository.hxx"

#include <drogon/orm/DbClient.h>
#include <sqlite/db-service.hxx>

using namespace scheduled_call_query;

ScheduledCallSchema ScheduledCallSchema::fromRow(const drogon::orm::Row& row)
{
  return {.id = row["id"].as<int64_t>(),
          .userId = row["user_id"].as<int64_t>(),
          .commandId = row["command_id"].as<std::string>(),
          .fireAt = row["fire_at"].as<int64_t>(),
          .topic = row["topic"].as<std::string>(),
          .lang = row["lang"].as<std::string>()};
}

drogon::Task<ScheduledCallCreateResult>
ScheduledCallRepository::create(const ScheduledCallCreateInput& input) const
{
  const auto client = DbService::client();
  const auto inserted = co_await client->execSqlCoro(std::string(INSERT), input.userId, input.commandId, input.fireAt, input.topic,
      input.lang, input.createdAt);
  if (!inserted.empty())
    co_return ScheduledCallCreateResult{
        .id = inserted.front()["id"].as<int64_t>(),
        .duplicate = false,
        .conflict = false};
  const auto existing =
      co_await client->execSqlCoro(std::string(FIND_BY_COMMAND), input.commandId);
  if (existing.empty())
    co_return ScheduledCallCreateResult{};
  const auto row = ScheduledCallSchema::fromRow(existing.front());
  const bool same = row.userId == input.userId && row.fireAt == input.fireAt &&
                    row.topic == input.topic;
  co_return ScheduledCallCreateResult{
      .id = row.id, .duplicate = same, .conflict = !same};
}

drogon::Task<int64_t> ScheduledCallRepository::pendingCount(int64_t userId) const
{
  const auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(std::string(COUNT_PENDING), userId);
  co_return rows.empty() ? 0 : rows.front()["total"].as<int64_t>();
}

drogon::Task<std::vector<ScheduledCallSchema>>
ScheduledCallRepository::due(const ScheduledCallDueInput& input) const
{
  const auto client = DbService::client();
  const auto rows =
      co_await client->execSqlCoro(std::string(FIND_DUE), input.now, input.limit);
  std::vector<ScheduledCallSchema> calls;
  calls.reserve(rows.size());
  for (const auto& row : rows)
    calls.push_back(ScheduledCallSchema::fromRow(row));
  co_return calls;
}

drogon::Task<bool> ScheduledCallRepository::markFired(int64_t id,
                                                      int64_t now) const
{
  const auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(MARK_FIRED), now, id);
  co_return result.affectedRows() > 0;
}
