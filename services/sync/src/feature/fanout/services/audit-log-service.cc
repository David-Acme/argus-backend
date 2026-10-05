#include "audit-log-service.hxx"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <stdexcept>
#include <text/json-util.hxx>
#include <unordered_map>
#include <unordered_set>

namespace
{
std::pair<int64_t, int64_t> utcDayRangeMs(int64_t nowMs)
{
  const std::time_t t = static_cast<std::time_t>(nowMs / 1000);
  std::tm tm{};
  gmtime_r(&t, &tm);
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  const int64_t start = static_cast<int64_t>(timegm(&tm)) * 1000;
  return {start, start + 86'400'000};
}
}

drogon::Task<AuditLogSchema>
AuditLogService::create(const AuditLogWriteInput& input) const
{
  const int64_t now = input.eventTimestamp.value_or(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto [dayStart, dayEnd] = utcDayRangeMs(now);

  const auto existing = co_await repository_.findExist(
      {.recordId = input.recordId,
       .tableName = input.tableName,
       .dayStart = dayStart,
       .dayEnd = dayEnd,
       .client = input.client});

  AuditLogSchema schema;
  if (!existing) {
    schema = co_await repository_.create(
        {.createUserId = input.createUserId,
         .recordId = input.recordId,
         .tableName = input.tableName,
         .changes = JsonDiff::toJson(input.changes),
         .priority = input.priority,
         .eventTimestamp = now,
         .client = input.client});
    co_return schema;
  }

  const auto prev =
      JsonDiff::fromJsonString(json_util::toString(existing->changes));
  const auto merged = JsonDiff::compareChanges(prev, input.changes);
  const auto changes = merged.type == "DELETE" ? input.changes : merged.changes;
  schema = co_await repository_.create(
      {.createUserId = input.createUserId ? input.createUserId
                                          : existing->createUserId,
       .recordId = input.recordId,
       .tableName = input.tableName,
       .changes = JsonDiff::toJson(changes),
       .priority = input.priority,
       .eventTimestamp = now,
       .client = input.client});
  co_await repository_.remove(existing->id, input.client);

  co_return schema;
}

drogon::Task<AuditCompactionRound>
AuditLogService::compact(const AuditCompactionStep& step) const
{
  const auto pairs = co_await repository_.findCompactionPairs(
      {.cutoffMs = step.cutoffMs, .afterId = step.afterId});
  if (pairs.empty())
    co_return AuditCompactionRound{};

  std::vector<int64_t> ids;
  ids.reserve(pairs.size() * 2);
  for (const auto& pair : pairs) {
    ids.push_back(pair.olderId);
    ids.push_back(pair.newerId);
  }
  const auto stored = co_await repository_.findCompactionChanges(ids);

  std::unordered_map<int64_t, Json::Value> pending;
  std::vector<int64_t> removeIds;
  removeIds.reserve(pairs.size());
  std::unordered_set<int64_t> removed;
  removed.reserve(pairs.size());
  int64_t frontier = 0;
  for (const auto& pair : pairs) {
    const auto olderIt = pending.find(pair.olderId);
    const Json::Value older = olderIt != pending.end()
                                  ? olderIt->second
                                  : stored.at(pair.olderId);
    const auto newerIt = pending.find(pair.newerId);
    const Json::Value newer = newerIt != pending.end()
                                  ? newerIt->second
                                  : stored.at(pair.newerId);
    const auto merged = JsonDiff::compareChanges(
        JsonDiff::fromJsonString(json_util::toString(older)),
        JsonDiff::fromJsonString(json_util::toString(newer)));
    pending[pair.newerId] =
        merged.type == "DELETE" ? newer : JsonDiff::toJson(merged.changes);
    removeIds.push_back(pair.olderId);
    removed.insert(pair.olderId);
    frontier = std::max(frontier, pair.olderId);
  }

  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    for (const auto& [id, changes] : pending) {
      if (removed.contains(id))
        continue;
      co_await repository_.compactRow(
          {.id = id, .changes = changes, .client = transaction.get()});
    }
    co_await repository_.removeMany(removeIds, transaction.get());
    if (frontier > 0)
      co_await repository_.advanceCompactionFrontier(frontier, transaction.get());
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw std::runtime_error("audit compaction round was not committed");
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  co_return AuditCompactionRound{
      .removed = static_cast<int64_t>(removeIds.size()),
      .lastOlderId = pairs.back().olderId};
}
