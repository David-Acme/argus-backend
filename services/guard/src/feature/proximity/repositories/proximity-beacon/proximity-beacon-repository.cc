#include "proximity-beacon-repository.hxx"

#include <sqlite/db-service.hxx>

#include <string>

using namespace proximity_beacon_query;

namespace
{
ProximityBeaconRow beaconOf(const drogon::orm::Row& row)
{
  return {.userId = row["user_id"].as<int64_t>(),
          .seed = row["seed"].as<std::string>(),
          .enabled = row["enabled"].as<int64_t>() != 0,
          .updatedAt = row["updated_at"].as<int64_t>()};
}
}

drogon::Task<std::optional<ProximityBeaconRow>> ProximityBeaconRepository::find(int64_t userId) const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(SELECT_BEACON), userId);
  if (rows.empty())
    co_return std::nullopt;
  co_return beaconOf(rows.front());
}

drogon::Task<std::vector<ProximityBeaconRow>> ProximityBeaconRepository::findEnabled() const
{
  const auto rows = co_await DbService::client()->execSqlCoro(std::string(SELECT_ENABLED));
  std::vector<ProximityBeaconRow> beacons;
  beacons.reserve(rows.size());
  for (const auto& row : rows)
    beacons.push_back(beaconOf(row));
  co_return beacons;
}

drogon::Task<void> ProximityBeaconRepository::upsert(const ProximityBeaconUpsertInput& input) const
{
  co_await DbService::client()->execSqlCoro(std::string(UPSERT_BEACON), input.userId, input.seed,
                                            static_cast<int64_t>(input.enabled ? 1 : 0), input.now, input.now);
}

drogon::Task<void> ProximityBeaconRepository::disable(int64_t userId, int64_t now) const
{
  co_await DbService::client()->execSqlCoro(std::string(DISABLE_BEACON), now, userId);
}
