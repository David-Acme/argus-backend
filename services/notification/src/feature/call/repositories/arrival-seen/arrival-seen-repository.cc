#include "arrival-seen-repository.hxx"

#include <drogon/orm/DbClient.h>
#include <sqlite/db-service.hxx>

using namespace arrival_seen_query;

drogon::Task<int64_t> ArrivalSeenRepository::lastSeen(int64_t personId) const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(FIND), personId);
  co_return rows.empty() ? 0 : rows.front()["last_seen"].as<int64_t>();
}

drogon::Task<void> ArrivalSeenRepository::touch(const ArrivalSeenInput& input) const
{
  co_await DbService::client()->execSqlCoro(std::string(UPSERT), input.personId,
                                            input.seenAt);
}

drogon::Task<int64_t> ArrivalSeenRepository::purgeStale(int64_t seenBefore) const
{
  const auto result = co_await DbService::client()->execSqlCoro(
      std::string(PURGE_STALE), seenBefore);
  co_return static_cast<int64_t>(result.affectedRows());
}
