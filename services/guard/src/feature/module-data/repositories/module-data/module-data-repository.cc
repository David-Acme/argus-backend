#include "module-data-repository.hxx"

using namespace module_data_query;

drogon::Task<ModuleDataSummary> ModuleDataRepository::summary(drogon::orm::DbClient* client) const
{
  const auto rows = co_await client->execSqlCoro(std::string(SUMMARY), kRowOverheadBytes);
  if (rows.empty())
    co_return ModuleDataSummary{};
  const auto& row = rows.front();
  co_return ModuleDataSummary{.items = {{.kind = "environments", .count = row["environments"].as<std::int64_t>()},
                                        {.kind = "episodes", .count = row["episodes"].as<std::int64_t>()},
                                        {.kind = "incidents", .count = row["incidents"].as<std::int64_t>()},
                                        {.kind = "decisions", .count = row["decisions"].as<std::int64_t>()},
                                        {.kind = "expected_guests", .count = row["guests"].as<std::int64_t>()},
                                        {.kind = "evidence_photos", .count = row["evidence"].as<std::int64_t>()}},
                              .bytes = row["bytes"].as<std::int64_t>()};
}

drogon::Task<void> ModuleDataRepository::purge(drogon::orm::DbClient* client) const
{
  for (const std::string_view statement : PURGE)
    co_await client->execSqlCoro(std::string(statement));
}

drogon::Task<std::vector<PendingEvidence>> ModuleDataRepository::pendingEvidence(drogon::orm::DbClient* client) const
{
  const auto rows = co_await client->execSqlCoro(std::string(PENDING_EVIDENCE), kEvidenceBatch);
  std::vector<PendingEvidence> pending;
  pending.reserve(rows.size());
  for (const auto& row : rows)
    pending.push_back({.id = row["id"].as<std::int64_t>(), .objectKey = row["object_key"].as<std::string>()});
  co_return pending;
}

drogon::Task<std::int64_t> ModuleDataRepository::countPendingEvidence(drogon::orm::DbClient* client) const
{
  const auto rows = co_await client->execSqlCoro(std::string(COUNT_PENDING_EVIDENCE));
  co_return rows.empty() ? 0 : rows.front()["total"].as<std::int64_t>();
}

drogon::Task<void> ModuleDataRepository::forgetEvidence(std::int64_t id, drogon::orm::DbClient* client) const
{
  co_await client->execSqlCoro(std::string(DELETE_EVIDENCE), id);
}
