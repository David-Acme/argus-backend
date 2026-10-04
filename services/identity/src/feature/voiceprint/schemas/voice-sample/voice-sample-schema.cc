#include "voice-sample-schema.hxx"

#include <drogon/orm/Field.h>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>

VoiceSampleSchema::VoiceSampleSchema(const drogon::orm::Row& row)
{
  id = row["id"].as<int64_t>();
  userId = row["user_id"].as<int64_t>();
  model = row["model"].as<std::string>();
  deviceHash = row["device_hash"].as<std::string>();
  embedding = voice_vector::fromBlob(row["embedding"].as<std::string>());
  turns = row["turns"].as<int>();
  speechSeconds = row["speech_seconds"].as<double>();
  state = voiceSampleStateFromString(row["state"].as<std::string>());
  createdAt = row["created_at"].as<int64_t>();
}
