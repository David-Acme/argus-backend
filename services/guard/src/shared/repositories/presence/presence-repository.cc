#include "presence-repository.hxx"

#include <algorithm>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>

#include <string>

using namespace presence_query;

namespace
{
PresenceRow fromRow(const drogon::orm::Row& row)
{
  return {.userId = row["user_id"].as<int64_t>(),
          .environmentId = row["environment_id"].as<int64_t>(),
          .state = presenceStateFromString(row["state"].as<std::string>()),
          .source = presenceSourceFromString(row["source"].as<std::string>()),
          .since = row["since"].as<int64_t>(),
          .lastHomeAt = row["last_home_at"].as<int64_t>(),
          .lastSignalAt = row["last_signal_at"].as<int64_t>()};
}

std::vector<PresenceRow> rowsOf(const drogon::orm::Result& result)
{
  std::vector<PresenceRow> rows;
  rows.reserve(result.size());
  for (const auto& row : result)
    rows.push_back(fromRow(row));
  return rows;
}

std::vector<int64_t> idsOf(const drogon::orm::Result& result,
                           const char* column)
{
  std::vector<int64_t> ids;
  ids.reserve(result.size());
  for (const auto& row : result)
    ids.push_back(row[column].as<int64_t>());
  return ids;
}
}

drogon::Task<std::vector<PresenceRow>>
PresenceRepository::forEnvironment(const PresenceLookupInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(FOR_ENVIRONMENT), input.environmentId);
  std::vector<PresenceRow> rows = rowsOf(result);
  if (!input.userIds.empty())
    std::erase_if(rows, [&input](const PresenceRow& row) {
      return std::ranges::find(input.userIds, row.userId) ==
             input.userIds.end();
    });
  co_return rows;
}

drogon::Task<std::vector<PresenceRow>>
PresenceRepository::forUser(int64_t userId) const
{
  const auto result =
      co_await DbService::client()->execSqlCoro(std::string(FOR_USER), userId);
  co_return rowsOf(result);
}

drogon::Task<std::vector<PresenceRow>> PresenceRepository::list() const
{
  const auto result =
      co_await DbService::client()->execSqlCoro(std::string(LIST_ALL));
  co_return rowsOf(result);
}

drogon::Task<std::optional<PresenceRow>>
PresenceRepository::find(const PresenceKey& key) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(FIND), key.userId, key.environmentId);
  if (result.empty())
    co_return std::nullopt;
  co_return fromRow(result.front());
}

drogon::Task<bool> PresenceRepository::upsert(const PresenceRow& row) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(UPSERT), row.userId, presenceStateToString(row.state),
      presenceSourceToString(row.source), row.since, row.lastHomeAt,
      row.lastSignalAt, row.environmentId);
  co_return result.affectedRows() > 0;
}

drogon::Task<PresenceDecision>
PresenceRepository::transition(const PresenceTransition& input) const
{
  auto transaction = co_await db_transaction::begin(DbService::client());
  PresenceDecision decision;
  try {
    const auto found = co_await transaction->execSqlCoro(
        std::string(FIND), input.key.userId, input.key.environmentId);
    std::optional<PresenceRow> current;
    if (!found.empty())
      current = fromRow(found.front());
    decision = input.decide(current);
    if (decision.write) {
      const PresenceRow& row = decision.row;
      const auto written = co_await transaction->execSqlCoro(
          std::string(UPSERT), row.userId, presenceStateToString(row.state),
          presenceSourceToString(row.source), row.since, row.lastHomeAt,
          row.lastSignalAt, row.environmentId);
      decision.write = written.affectedRows() > 0;
    }
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  if (!co_await db_transaction::Commit(std::move(transaction)))
    decision.write = false;
  co_return decision;
}

drogon::Task<std::vector<int64_t>>
PresenceRepository::removeUser(int64_t userId) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(REMOVE_USER), userId);
  co_return idsOf(result, "environment_id");
}

drogon::Task<std::vector<int64_t>> PresenceRepository::userIds() const
{
  const auto result =
      co_await DbService::client()->execSqlCoro(std::string(USER_IDS));
  co_return idsOf(result, "user_id");
}

drogon::Task<std::vector<int64_t>> PresenceRepository::lanEnvironments() const
{
  const auto result =
      co_await DbService::client()->execSqlCoro(std::string(LAN_ENVIRONMENTS));
  co_return idsOf(result, "id");
}

drogon::Task<std::vector<PresenceRow>>
PresenceRepository::expireHome(const PresenceExpireInput& input) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(EXPIRE_HOME), input.at, input.at, input.homeBefore);
  co_return rowsOf(result);
}

drogon::Task<int64_t> PresenceRepository::purgeStale(int64_t signalBefore) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(PURGE_STALE), signalBefore);
  co_return static_cast<int64_t>(result.affectedRows());
}
