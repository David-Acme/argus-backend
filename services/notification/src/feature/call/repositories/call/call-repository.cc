#include "call-repository.hxx"

#include <drogon/orm/DbClient.h>
#include <sqlite/db-service.hxx>

using namespace call_query;

namespace
{
drogon::orm::DbClient* executor(drogon::orm::DbClient* given, const drogon::orm::DbClientPtr& pool)
{
  return given != nullptr ? given : pool.get();
}

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
      input.createdAt, input.expiresAt, input.pushAfter);
  if (rows.empty())
    co_return std::nullopt;
  co_return rows.front()["id"].as<int64_t>();
}

drogon::Task<std::optional<int64_t>>
CallRepository::createRinging(const CallCreateInput& input) const
{
  const auto pool = DbService::client();
  const auto rows = co_await executor(input.client, pool)->execSqlCoro(std::string(INSERT_RINGING), input.userId, input.dedupeKey,
      callTriggerToString(input.trigger), input.reason, input.urgency,
      input.lang, input.title, input.summary, input.openingLine,
      input.missedLine, input.data, input.createdAt, input.expiresAt,
      input.pushAfter, input.userId, input.createdAt);
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
CallRepository::findById(int64_t id, drogon::orm::DbClient* given) const
{
  const auto pool = DbService::client();
  const auto rows = co_await executor(given, pool)->execSqlCoro(std::string(FIND_BY_ID), id);
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
  const auto pool = DbService::client();
  const auto result = co_await executor(input.client, pool)->execSqlCoro(std::string(CLAIM), input.now, input.sessionId,
      input.id, input.userId, input.now);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> CallRepository::markMissed(const CallMissedInput& input) const
{
  const auto pool = DbService::client();
  const auto result = co_await executor(input.client, pool)->execSqlCoro(std::string(MARK_MISSED), input.now, input.id);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> CallRepository::end(const CallEndInput& input) const
{
  const auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(END), callStateToString(input.state), input.now, input.id,
      input.userId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> CallRepository::declineRinging(const CallDeclineInput& input) const
{
  const auto pool = DbService::client();
  const auto result = co_await executor(input.client, pool)->execSqlCoro(std::string(DECLINE_RINGING), input.now,
      input.id, input.userId, input.now);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> CallRepository::markPushed(const CallPushedInput& input) const
{
  const auto pool = DbService::client();
  const auto result = co_await executor(input.client, pool)->execSqlCoro(std::string(MARK_PUSHED), input.now, input.id);
  co_return result.affectedRows() > 0;
}

drogon::Task<std::vector<CallSchema>>
CallRepository::findFollowups(int64_t parentId, drogon::orm::DbClient* given) const
{
  const auto pool = DbService::client();
  co_return rowsOf(co_await executor(given, pool)->execSqlCoro(std::string(FIND_FOLLOWUPS), parentId));
}

drogon::Task<int64_t>
CallRepository::settleFollowups(const CallSettleInput& input) const
{
  const auto pool = DbService::client();
  const auto result = co_await executor(input.client, pool)->execSqlCoro(std::string(SETTLE_FOLLOWUPS),
      callStateToString(input.state), input.now, input.parentId);
  co_return static_cast<int64_t>(result.affectedRows());
}

drogon::Task<std::vector<CallSchema>> CallRepository::findRinging() const
{
  const auto client = DbService::client();
  co_return rowsOf(co_await client->execSqlCoro(std::string(FIND_RINGING)));
}

drogon::Task<std::vector<CallSchema>> CallRepository::findStale(const StaleCallsInput& input) const
{
  const auto client = DbService::client();
  co_return rowsOf(co_await client->execSqlCoro(std::string(FIND_STALE), input.answeredBefore,
                                                input.createdBefore));
}

drogon::Task<std::vector<int64_t>> CallRepository::closeStaleByIds(const CloseStaleIdsInput& input) const
{
  if (input.ids.empty())
    co_return std::vector<int64_t>{};
  std::string sql(CLOSE_STALE_PREFIX);
  std::vector<std::string> args{std::to_string(input.now)};
  for (std::size_t index = 0; index < input.ids.size(); ++index) {
    if (index > 0)
      sql += ", ";
    sql += "?";
    args.push_back(std::to_string(input.ids[index]));
  }
  sql += ") RETURNING id";
  const auto pool = DbService::client();
  drogon::orm::DbClient* executor = input.client != nullptr ? input.client : pool.get();
  const auto& argsRef = args;
  const auto rows = co_await executor->execSqlCoro(sql, argsRef);
  std::vector<int64_t> closed;
  closed.reserve(rows.size());
  for (const auto& row : rows)
    closed.push_back(row["id"].as<int64_t>());
  co_return closed;
}

drogon::Task<std::vector<CallCancelled>>
CallRepository::cancelRingingForKey(const CallCancelForKeyInput& input) const
{
  const auto pool = DbService::client();
  const auto rows = co_await executor(input.client, pool)->execSqlCoro(std::string(CANCEL_RINGING_FOR_KEY),
                                                 input.reason, input.now,
                                                 input.dedupeKey, input.exceptUserId,
                                                 input.onlyUserId, input.onlyUserId);
  std::vector<CallCancelled> cancelled;
  cancelled.reserve(rows.size());
  for (const auto& row : rows)
    cancelled.push_back({.id = row["id"].as<int64_t>(),
                         .userId = row["user_id"].as<int64_t>()});
  co_return cancelled;
}

drogon::Task<int64_t> CallRepository::purgeSettled(int64_t createdBefore) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(PURGE_SETTLED), createdBefore);
  co_return static_cast<int64_t>(result.affectedRows());
}
