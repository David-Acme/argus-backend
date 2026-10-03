#include "candidate-retention-repository.hxx"

#include <json/value.h>
#include <text/json-util.hxx>

using namespace candidate_retention_query;

namespace
{

std::string idList(const std::vector<RetiredCandidate>& retired)
{
  Json::Value ids(Json::arrayValue);
  for (const auto& candidate : retired)
    ids.append(static_cast<Json::Int64>(candidate.id));
  return json_util::toString(ids);
}

}

drogon::Task<std::vector<RetiredCandidate>>
CandidateRetentionRepository::retireStale(const CandidateRetireInput& input) const
{
  const auto result = co_await input.client->execSqlCoro(
      RETIRE_STALE.data(), input.cutoff, input.limit);
  std::vector<RetiredCandidate> retired;
  retired.reserve(result.size());
  for (const auto& row : result)
    retired.push_back({.id = row["id"].as<int64_t>(),
                       .deletedAt = row["deleted_at"].as<int64_t>()});
  co_return retired;
}

drogon::Task<RetiredBiometrics> CandidateRetentionRepository::purgeBiometrics(
    const std::vector<RetiredCandidate>& retired,
    drogon::orm::DbClient* client) const
{
  const std::string ids = idList(retired);
  RetiredBiometrics purged;
  const auto embeddings = co_await client->execSqlCoro(EMBEDDING_IDS.data(), ids);
  purged.embeddingIds.reserve(embeddings.size());
  for (const auto& row : embeddings)
    purged.embeddingIds.push_back(row["id"].as<int64_t>());
  co_await client->execSqlCoro(DELETE_EMBEDDINGS.data(), ids);
  co_await client->execSqlCoro(DELETE_SNAPSHOTS.data(), ids);
  co_await client->execSqlCoro(DELETE_TAGS.data(), ids);
  co_return purged;
}
