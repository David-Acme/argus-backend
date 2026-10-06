#include "module-data-repository.hxx"

#include <string>

using namespace module_data_query;

drogon::Task<ModuleDataSummary> ModuleDataRepository::summary(drogon::orm::DbClient* client) const
{
  const auto rows = co_await client->execSqlCoro(std::string(SUMMARY), kRowOverheadBytes);
  if (rows.empty())
    co_return ModuleDataSummary{};
  const auto& row = rows.front();
  co_return ModuleDataSummary{.items = {{.kind = "projects", .count = row["projects"].as<std::int64_t>()},
                                        {.kind = "tasks", .count = row["tasks"].as<std::int64_t>()},
                                        {.kind = "project_members", .count = row["members"].as<std::int64_t>()},
                                        {.kind = "calendar_events", .count = row["events"].as<std::int64_t>()},
                                        {.kind = "calendar_shares", .count = row["shares"].as<std::int64_t>()}},
                              .bytes = row["bytes"].as<std::int64_t>()};
}

drogon::Task<void> ModuleDataRepository::purge(drogon::orm::DbClient* client) const
{
  for (const std::string_view statement : PURGE)
    co_await client->execSqlCoro(std::string(statement));
}
