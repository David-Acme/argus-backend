#include "guard-site-repository.hxx"

#include <sqlite/db-service.hxx>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace guard_site_query;

namespace
{
GuardSite fromRow(const drogon::orm::Row& row)
{
  return {.profile = siteProfileFromString(row["profile"].as<std::string>())
                         .value_or(SiteProfile::Home),
          .scheduleEnabled = row["schedule_enabled"].as<int>() != 0,
          .asleep = row["asleep_hours"].as<std::string>(),
          .open = row["open_hours"].as<std::string>(),
          .staffed = row["staffed_hours"].as<std::string>(),
          .closedMode = guardModeFromString(row["closed_mode"].as<std::string>()),
          .digestHour = row["digest_hour"].as<int>(),
          .updatedAt = row["updated_at"].as<int64_t>()};
}

std::string closedModeName(GuardMode mode)
{
  return mode == GuardMode::Armed ? "armed" : "away";
}
}

drogon::Task<std::optional<GuardSite>> GuardSiteRepository::find() const
{
  const auto rows =
      co_await DbService::client()->execSqlCoro(std::string(SELECT_SITE));
  if (rows.empty())
    co_return std::nullopt;
  co_return fromRow(rows.front());
}

drogon::Task<GuardSite>
GuardSiteRepository::update(const GuardSiteUpdateInput& input) const
{
  const auto client = DbService::client();
  const GuardSite& seed = input.seed;
  co_await client->execSqlCoro(
      std::string(SEED_SITE), siteProfileToString(seed.profile),
      seed.scheduleEnabled ? 1 : 0, seed.asleep, seed.open, seed.staffed,
      closedModeName(seed.closedMode), seed.digestHour, input.updatedAt);

  std::string sql(UPDATE_SITE_PREFIX);
  std::vector<std::string> args{std::to_string(input.updatedAt)};
  const auto addField = [&sql, &args](std::string_view column,
                                      std::string value) {
    sql += column;
    args.push_back(std::move(value));
  };
  if (input.profile)
    addField(UPDATE_COL_PROFILE, siteProfileToString(*input.profile));
  if (input.scheduleEnabled)
    addField(UPDATE_COL_SCHEDULE_ENABLED, *input.scheduleEnabled ? "1" : "0");
  if (input.asleep)
    addField(UPDATE_COL_ASLEEP, *input.asleep);
  if (input.open)
    addField(UPDATE_COL_OPEN, *input.open);
  if (input.staffed)
    addField(UPDATE_COL_STAFFED, *input.staffed);
  if (input.closedMode)
    addField(UPDATE_COL_CLOSED_MODE, closedModeName(*input.closedMode));
  if (input.digestHour)
    addField(UPDATE_COL_DIGEST_HOUR, std::to_string(*input.digestHour));
  sql += UPDATE_SITE_SUFFIX;
  const auto& argsRef = args;
  co_await client->execSqlCoro(sql, argsRef);

  const auto updated = co_await find();
  if (!updated)
    throw std::runtime_error("guard site update lost its row");
  co_return *updated;
}
