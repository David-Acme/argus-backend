#include "voiceprint-challenge-sample-schema.hxx"

#include <drogon/orm/Field.h>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>

VoiceprintChallengeSampleSchema::VoiceprintChallengeSampleSchema(
    const drogon::orm::Row& row)
{
  challengeId = row["challenge_id"].as<int64_t>();
  position = row["position"].as<int>();
  embedding = voice_vector::fromBlob(row["embedding"].as<std::string>());
  speechSeconds = row["speech_seconds"].as<double>();
  createdAt = row["created_at"].as<int64_t>();
}
