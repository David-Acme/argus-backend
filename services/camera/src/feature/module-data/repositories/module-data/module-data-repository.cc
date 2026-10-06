#include "module-data-repository.hxx"

using namespace module_data_query;

drogon::Task<ModuleDataSummary> ModuleDataRepository::summary(drogon::orm::DbClient* client) const
{
  const auto rows = co_await client->execSqlCoro(std::string(SUMMARY), kRowOverheadBytes);
  if (rows.empty())
    co_return ModuleDataSummary{};
  const auto& row = rows.front();
  co_return ModuleDataSummary{.items = {{.kind = "cameras", .count = row["cameras"].as<std::int64_t>()},
                                        {.kind = "zones", .count = row["zones"].as<std::int64_t>()},
                                        {.kind = "evidence_photos", .count = row["evidence"].as<std::int64_t>()},
                                        {.kind = "camera_actions", .count = row["actions"].as<std::int64_t>()}},
                              .bytes = row["bytes"].as<std::int64_t>()};
}

drogon::Task<std::vector<std::int64_t>> ModuleDataRepository::purge(drogon::orm::DbClient* client) const
{
  const auto cameras = co_await client->execSqlCoro(std::string(CAMERA_IDS));
  std::vector<std::int64_t> ids;
  ids.reserve(cameras.size());
  for (const auto& row : cameras)
    ids.push_back(row["id"].as<std::int64_t>());
  for (const std::string_view statement : PURGE)
    co_await client->execSqlCoro(std::string(statement));
  co_return ids;
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
