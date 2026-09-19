#include "person-snapshot-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace person_snapshot_query;

drogon::Task<bool> PersonSnapshotRepository::store(
    const PersonSnapshotStoreInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      UPSERT.data(), input.personId, input.image,
      static_cast<int64_t>(std::time(nullptr)));
  co_return result.affectedRows() > 0;
}

drogon::Task<std::optional<std::string>>
PersonSnapshotRepository::findImage(int64_t personId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_PERSON.data(), personId);
  if (result.empty())
    co_return std::nullopt;
  co_return result.front()["image"].as<std::string>();
}
