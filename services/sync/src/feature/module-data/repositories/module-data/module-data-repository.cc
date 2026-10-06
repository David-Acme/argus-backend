#include "module-data-repository.hxx"

using namespace module_data_query;

drogon::Task<ModuleDataSummary> ModuleDataRepository::summary(const ModuleHistoryInput& input) const
{
  const auto rows = co_await input.client->execSqlCoro(std::string(SUMMARY), input.tables, kRowOverheadBytes);
  if (rows.empty())
    co_return ModuleDataSummary{};
  const auto& row = rows.front();
  co_return ModuleDataSummary{.items = {{.kind = "change_history", .count = row["history"].as<std::int64_t>()}},
                              .bytes = row["bytes"].as<std::int64_t>()};
}

drogon::Task<void> ModuleDataRepository::purge(const ModuleHistoryInput& input) const
{
  for (const std::string_view statement : PURGE)
    co_await input.client->execSqlCoro(std::string(statement), input.tables);
}
