#include "voice-sample-repository.hxx"

#include <sqlite/db-service.hxx>
#include <string>

using namespace voice_sample_query;

namespace
{

std::string withIds(const char* prefix, const std::vector<int64_t>& ids)
{
  std::string sql(prefix);
  for (size_t index = 0; index < ids.size(); ++index) {
    if (index > 0)
      sql += ", ";
    sql += std::to_string(ids[index]);
  }
  sql += ")";
  return sql;
}

std::vector<VoiceSampleSchema> samplesOf(const drogon::orm::Result& rows)
{
  std::vector<VoiceSampleSchema> samples;
  samples.reserve(rows.size());
  for (const auto& row : rows)
    samples.emplace_back(row);
  return samples;
}

}

drogon::Task<int64_t>
VoiceSampleRepository::create(const VoiceSampleCreateInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(
      INSERT, input.userId, input.model, input.deviceHash, input.embedding,
      input.turns, input.speechSeconds, voiceSampleStateToString(input.state),
      input.createdAt);
  co_return static_cast<int64_t>(result.insertId());
}

drogon::Task<std::vector<VoiceSampleSchema>>
VoiceSampleRepository::findSince(const VoiceSampleWindowInput& input) const
{
  const auto client = DbService::identityClient();
  const auto rows = co_await client->execSqlCoro(FIND_SINCE, input.userId,
                                                 input.model, input.since);
  co_return samplesOf(rows);
}

drogon::Task<std::vector<VoiceSampleSchema>>
VoiceSampleRepository::findAdopted(const VoiceSampleAdoptedInput& input) const
{
  const auto client = DbService::identityClient();
  const auto rows = co_await client->execSqlCoro(FIND_ADOPTED, input.userId,
                                                 input.model, input.limit);
  co_return samplesOf(rows);
}

drogon::Task<int>
VoiceSampleRepository::countAdoptedAfter(const VoiceSampleCountInput& input) const
{
  const auto client = DbService::identityClient();
  const auto rows = co_await client->execSqlCoro(
      COUNT_ADOPTED_SINCE, input.userId, input.model, input.after);
  co_return rows.empty() ? 0 : rows.front()["total"].as<int>();
}

drogon::Task<void>
VoiceSampleRepository::adopt(const VoiceSampleAdoptInput& input) const
{
  if (input.ids.empty())
    co_return;
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  co_await client->execSqlCoro(withIds(ADOPT_PREFIX, input.ids), input.userId);
}

drogon::Task<void>
VoiceSampleRepository::dropAdoptedExcept(const VoiceSampleAdoptInput& input) const
{
  if (input.ids.empty())
    co_return;
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  co_await client->execSqlCoro(withIds(DROP_ADOPTED_PREFIX, input.ids),
                               input.userId);
}

drogon::Task<void>
VoiceSampleRepository::prune(const VoiceSamplePruneInput& input) const
{
  const auto client = DbService::identityClient();
  co_await client->execSqlCoro(PRUNE_OTHER_MODELS, input.userId, input.model);
  const std::string pending = voiceSampleStateToString(VoiceSampleState::Pending);
  const std::string adopted = voiceSampleStateToString(VoiceSampleState::Adopted);
  co_await client->execSqlCoro(PRUNE_OVERFLOW, input.userId, input.model,
                               pending, input.userId, input.model, pending,
                               input.maxPending);
  co_await client->execSqlCoro(PRUNE_OVERFLOW, input.userId, input.model,
                               adopted, input.userId, input.model, adopted,
                               input.maxAdopted);
  co_await purgeBefore(input.pendingBefore, input.adoptedBefore);
}

drogon::Task<void> VoiceSampleRepository::purgeBefore(int64_t pendingBefore,
                                                      int64_t adoptedBefore) const
{
  const auto client = DbService::identityClient();
  co_await client->execSqlCoro(PURGE_PENDING_BEFORE, pendingBefore);
  co_await client->execSqlCoro(PURGE_ADOPTED_BEFORE, adoptedBefore);
}

drogon::Task<size_t>
VoiceSampleRepository::removeByUser(int64_t userId,
                                    drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto result = co_await resolved->execSqlCoro(DELETE_BY_USER, userId);
  co_return result.affectedRows();
}
