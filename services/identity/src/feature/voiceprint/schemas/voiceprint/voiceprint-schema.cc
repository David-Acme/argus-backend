#include "voiceprint-schema.hxx"

#include <drogon/orm/Field.h>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>

VoiceprintSchema::VoiceprintSchema(const drogon::orm::Row& row)
{
  id = row["id"].as<int64_t>();
  userId = row["user_id"].as<int64_t>();
  model = row["model"].as<std::string>();
  embedding = voice_vector::fromBlob(row["embedding"].as<std::string>());
  sampleCount = row["sample_count"].as<int>();
  speechSeconds = row["speech_seconds"].as<double>();
  method = voiceprintMethodFromString(row["method"].as<std::string>());
  consentVersion = row["consent_version"].as<std::string>();
  if (!row["enrolled_by"].isNull())
    enrolledBy = row["enrolled_by"].as<int64_t>();
  createdAt = row["created_at"].as<int64_t>();
}
