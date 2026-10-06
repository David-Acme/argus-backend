#include "module-data-repository.hxx"

#include <json/value.h>
#include <text/json-util.hxx>

using namespace module_data_query;

drogon::Task<ModuleDataSummary> ModuleDataRepository::summary(drogon::orm::DbClient* client) const
{
  const auto rows = co_await client->execSqlCoro(std::string(SUMMARY), kRowOverheadBytes);
  if (rows.empty())
    co_return ModuleDataSummary{};
  const auto& row = rows.front();
  co_return ModuleDataSummary{.items = {{.kind = "visitors", .count = row["visitors"].as<std::int64_t>()},
                                        {.kind = "visitor_face_samples", .count = row["samples"].as<std::int64_t>()},
                                        {.kind = "visits", .count = row["visits"].as<std::int64_t>()}},
                              .bytes = row["bytes"].as<std::int64_t>()};
}

drogon::Task<PurgedVisitors> ModuleDataRepository::purgeVisitors(drogon::orm::DbClient* client) const
{
  const auto visitors = co_await client->execSqlCoro(std::string(VISITOR_IDS));
  Json::Value ids(Json::arrayValue);
  for (const auto& row : visitors)
    ids.append(static_cast<Json::Int64>(row["id"].as<std::int64_t>()));
  const std::string idList = json_util::toString(ids);
  PurgedVisitors purged{.visitors = static_cast<std::int64_t>(visitors.size()), .embeddingIds = {}, .cropKeys = {}};
  const auto embeddings = co_await client->execSqlCoro(std::string(EMBEDDING_IDS), idList);
  purged.embeddingIds.reserve(embeddings.size());
  for (const auto& row : embeddings)
    purged.embeddingIds.push_back(row["id"].as<std::int64_t>());
  const auto crops = co_await client->execSqlCoro(std::string(CROP_KEYS), idList);
  purged.cropKeys.reserve(crops.size());
  for (const auto& row : crops)
    purged.cropKeys.push_back(row["crop_key"].as<std::string>());
  for (const std::string_view statement : DELETE_VISITORS)
    co_await client->execSqlCoro(std::string(statement), idList);
  co_await client->execSqlCoro(std::string(RESET_COUNTER));
  co_return purged;
}
