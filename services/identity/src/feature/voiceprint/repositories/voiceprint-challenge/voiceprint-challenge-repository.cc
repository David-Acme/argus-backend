#include "voiceprint-challenge-repository.hxx"

#include <ctime>
#include <sqlite/db-service.hxx>

using namespace voiceprint_challenge_query;

drogon::Task<VoiceprintChallengeSchema> VoiceprintChallengeRepository::create(
    const VoiceprintChallengeCreateInput& input) const
{
  auto client = DbService::identityClient();
  const auto result =
      co_await client->execSqlCoro(INSERT, input.tokenHash, input.userId,
                                   input.requesterId, input.deviceHash,
                                   voiceLangToString(input.lang), input.phrases,
                                   input.expiresAt);

  VoiceprintChallengeSchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
  schema.tokenHash = input.tokenHash;
  schema.userId = input.userId;
  schema.requesterId = input.requesterId;
  schema.deviceHash = input.deviceHash;
  schema.lang = input.lang;
  schema.phrases = input.phrases;
  schema.expiresAt = input.expiresAt;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<VoiceprintChallengeSchema>>
VoiceprintChallengeRepository::findByTokenHash(
    const std::string& tokenHash, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto rows =
      co_await resolved->execSqlCoro(FIND_BY_TOKEN_HASH, tokenHash);
  if (rows.empty())
    co_return std::nullopt;
  co_return VoiceprintChallengeSchema(rows.front());
}

drogon::Task<bool> VoiceprintChallengeRepository::tryConsume(
    const VoiceprintChallengeConsumeInput& input) const
{
  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto result =
      co_await client->execSqlCoro(TRY_CONSUME, input.id, input.now);
  co_return result.affectedRows() == 1;
}

drogon::Task<void>
VoiceprintChallengeRepository::purgeExpired(int64_t now) const
{
  auto client = DbService::identityClient();
  co_await client->execSqlCoro(PURGE_SAMPLES, now);
  co_await client->execSqlCoro(PURGE, now);
}

drogon::Task<void> VoiceprintChallengeRepository::stageSample(
    const VoiceprintStageSampleInput& input) const
{
  auto client = DbService::identityClient();
  co_await client->execSqlCoro(STAGE_SAMPLE, input.challengeId, input.position,
                               input.embedding, input.speechSeconds);
}

drogon::Task<std::vector<VoiceprintChallengeSampleSchema>>
VoiceprintChallengeRepository::findSamples(int64_t challengeId,
                                           drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  const auto rows = co_await resolved->execSqlCoro(FIND_SAMPLES, challengeId);
  std::vector<VoiceprintChallengeSampleSchema> samples;
  samples.reserve(rows.size());
  for (const auto& row : rows)
    samples.emplace_back(row);
  co_return samples;
}

drogon::Task<void>
VoiceprintChallengeRepository::deleteSamples(int64_t challengeId,
                                             drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::identityClient();
  auto* resolved = client ? client : pooled.get();
  co_await resolved->execSqlCoro(DELETE_SAMPLES, challengeId);
}
