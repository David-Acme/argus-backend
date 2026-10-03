#include "voiceprint-challenge-schema.hxx"

#include <drogon/orm/Field.h>

VoiceprintChallengeSchema::VoiceprintChallengeSchema(
    const drogon::orm::Row& row)
{
  id = row["id"].as<int64_t>();
  tokenHash = row["token_hash"].as<std::string>();
  userId = row["user_id"].as<int64_t>();
  requesterId = row["requester_id"].as<int64_t>();
  deviceHash = row["device_hash"].as<std::string>();
  lang = voiceLangFromString(row["lang"].as<std::string>());
  phrases = row["phrases"].as<std::string>();
  expiresAt = row["expires_at"].as<int64_t>();
  if (!row["consumed_at"].isNull())
    consumedAt = row["consumed_at"].as<int64_t>();
  createdAt = row["created_at"].as<int64_t>();
}
