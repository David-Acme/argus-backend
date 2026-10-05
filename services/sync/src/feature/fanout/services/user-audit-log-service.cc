#include "user-audit-log-service.hxx"

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

drogon::Task<std::vector<UserAuditLogSchema>>
UserAuditLogService::createMany(const UserAuditLogBatchWriteInput& input) const
{
  std::vector<int64_t> recipients;
  recipients.reserve(input.userIds.size());
  std::unordered_set<int64_t> seen;
  seen.reserve(input.userIds.size());
  for (const int64_t userId : input.userIds) {
    if (userId > 0 && seen.insert(userId).second)
      recipients.push_back(userId);
  }
  if (recipients.empty())
    co_return {};

  const int64_t now = input.eventTimestamp.value_or(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto [dayStart, dayEnd] = utcDayRangeMs(now);

  const auto existing = co_await repository_.findExistMany(
      {.userIds = recipients,
       .recordId = input.recordId,
       .tableName = input.tableName,
       .dayStart = dayStart,
       .dayEnd = dayEnd,
       .client = input.client});
  std::unordered_map<int64_t, const UserAuditLogSchema*> existingByUser;
  existingByUser.reserve(existing.size());
  std::vector<int64_t> supersededIds;
  supersededIds.reserve(existing.size());
  for (const auto& row : existing) {
    existingByUser.emplace(row.userId, &row);
    supersededIds.push_back(row.id);
  }

  const Json::Value fresh = JsonDiff::toJson(input.changes);
  std::vector<UserAuditLogCreateInput> rows;
  rows.reserve(recipients.size());
  for (const int64_t userId : recipients) {
    Json::Value changes = fresh;
    if (const auto found = existingByUser.find(userId);
        found != existingByUser.end()) {
      const auto merged = JsonDiff::compareChanges(
          JsonDiff::fromJsonString(json_util::toString(found->second->changes)),
          input.changes);
      if (merged.type != "DELETE")
        changes = JsonDiff::toJson(merged.changes);
    }
    rows.push_back({.userId = userId,
                    .recordId = input.recordId,
                    .tableName = input.tableName,
                    .changes = std::move(changes),
                    .priority = input.priority,
                    .eventTimestamp = now});
  }

  auto written =
      co_await repository_.createMany({.rows = rows, .client = input.client});
  co_await repository_.removeMany(supersededIds, input.client);
  co_return written;
}

drogon::Task<AuditCompactionRound>
UserAuditLogService::compact(const AuditCompactionStep& step) const
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
