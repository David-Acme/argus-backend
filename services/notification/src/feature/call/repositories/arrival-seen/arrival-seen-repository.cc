#include "arrival-seen-repository.hxx"

#include <drogon/orm/DbClient.h>
#include <sqlite/db-service.hxx>

using namespace arrival_seen_query;

drogon::Task<int64_t>
ArrivalSeenRepository::touch(const ArrivalSeenInput& input) const
{
  const auto client = DbService::client();
  const auto rows = co_await client->execSqlCoro(std::string(FIND), input.personId);
  const int64_t previous =
      rows.empty() ? 0 : rows.front()["last_seen"].as<int64_t>();
  co_await client->execSqlCoro(std::string(UPSERT), input.personId, input.seenAt);
  co_return previous;
}
