#include "voice-profile-schema.hxx"

#include <drogon/orm/Field.h>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>

VoiceProfileSchema::VoiceProfileSchema(const drogon::orm::Row& row)
{
  id = row["id"].as<int64_t>();
  userId = row["user_id"].as<int64_t>();
  model = row["model"].as<std::string>();
  embedding = voice_vector::fromBlob(row["embedding"].as<std::string>());
  sampleCount = row["sample_count"].as<int>();
  speechSeconds = row["speech_seconds"].as<double>();
  source = voiceProfileSourceFromString(row["source"].as<std::string>());
  linkedAt = row["linked_at"].as<int64_t>();
  refreshedAt = row["refreshed_at"].as<int64_t>();
  createdAt = row["created_at"].as<int64_t>();
}
