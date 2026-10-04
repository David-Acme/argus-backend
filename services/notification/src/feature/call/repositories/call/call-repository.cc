#include "call-repository.hxx"

#include <drogon/orm/DbClient.h>
#include <sqlite/db-service.hxx>

using namespace call_query;

namespace
{
std::vector<CallSchema> rowsOf(const drogon::orm::Result& result)
{
  std::vector<CallSchema> calls;
  calls.reserve(result.size());
  for (const auto& row : result)
    calls.push_back(CallSchema::fromRow(row));
  return calls;
}
}

drogon::Task<std::optional<int64_t>>
CallRepository::create(const CallCreateInput& input) const
{
  const auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(std::string(INSERT), input.userId, input.dedupeKey,
      callTriggerToString(input.trigger), callStateToString(input.state),
      input.reason, input.parentCallId, input.urgency, input.lang, input.title,
      input.summary, input.openingLine, input.missedLine, input.data,
      input.createdAt, input.expiresAt);
  if (rows.empty())
    co_return std::nullopt;
  co_return rows.front()["id"].as<int64_t>();
}

drogon::Task<std::optional<int64_t>>
CallRepository::createRinging(const CallCreateInput& input) const
{
  const auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(std::string(INSERT_RINGING), input.userId, input.dedupeKey,
      callTriggerToString(input.trigger), input.reason, input.urgency,
      input.lang, input.title, input.summary, input.openingLine,
      input.missedLine, input.data, input.createdAt, input.expiresAt,
      input.userId, input.createdAt);
  if (rows.empty())
    co_return std::nullopt;
  co_return rows.front()["id"].as<int64_t>();
}

drogon::Task<bool> CallRepository::exists(const std::string& dedupeKey,
                                          int64_t userId) const
{
  const auto client = DbService::client();
  const auto rows =
      co_await client->execSqlCoro(std::string(EXISTS), dedupeKey, userId);
  co_return !rows.empty();
}

drogon::Task<std::optional<CallSchema>>
CallRepository::findById(int64_t id) const
{
  const auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(std::string(FIND_BY_ID), id);
  if (rows.empty())
    co_return std::nullopt;
  co_return CallSchema::fromRow(rows.front());
}

drogon::Task<std::optional<CallSchema>>
CallRepository::findRingingFor(const CallRingingInput& input) const
{
  const auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(std::string(FIND_RINGING_FOR),
                                                 input.userId, input.now);
  if (rows.empty())
    co_return std::nullopt;
  co_return CallSchema::fromRow(rows.front());
}

drogon::Task<CallRingStats>
CallRepository::ringStats(const CallRingStatsInput& input) const
{
  const auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(std::string(RING_STATS),
                                                 input.since, input.userId);
  if (rows.empty())
    co_return CallRingStats{};
  co_return CallRingStats{.lastAt = rows.front()["last_at"].as<int64_t>(),
                          .recent = rows.front()["recent"].as<int>()};
}

drogon::Task<bool> CallRepository::claim(const CallClaimInput& input) const
{
  const auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(CLAIM), input.now, input.sessionId, input.id, input.userId,
      input.now);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> CallRepository::markMissed(int64_t id, int64_t now) const
{
  const auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(MARK_MISSED), now, id);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> CallRepository::end(const CallEndInput& input) const
{
  const auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(END), callStateToString(input.state), input.now, input.id,
      input.userId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> CallRepository::markPushed(int64_t id, int64_t now) const
{
  const auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(MARK_PUSHED), now, id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<CallSchema>>
CallRepository::findFollowups(int64_t parentId) const
{
  const auto client = DbService::client();
  co_return rowsOf(co_await client->execSqlCoro(std::string(FIND_FOLLOWUPS), parentId));
}

drogon::Task<int64_t>
CallRepository::settleFollowups(const CallSettleInput& input) const
{
  const auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(SETTLE_FOLLOWUPS), callStateToString(input.state), input.now,
      input.parentId);
  co_return static_cast<int64_t>(result.affectedRows());
}

drogon::Task<std::vector<CallSchema>> CallRepository::findRinging() const
{
  const auto client = DbService::client();
  co_return rowsOf(co_await client->execSqlCoro(std::string(FIND_RINGING)));
}

drogon::Task<int64_t> CallRepository::closeStaleAnswered(int64_t answeredBefore,
                                                         int64_t now) const
{
  const auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(CLOSE_STALE_ANSWERED),
                                                   now, answeredBefore);
  co_return static_cast<int64_t>(result.affectedRows());
}
