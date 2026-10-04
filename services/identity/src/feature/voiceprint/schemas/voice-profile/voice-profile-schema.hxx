#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <feature/voiceprint/vocabulary/voice-profile-source.hxx>
#include <string>
#include <vector>

struct VoiceProfileSchema
{
  int64_t id{0};
  int64_t userId{0};
  std::string model;
  std::vector<float> embedding;
  int sampleCount{0};
  double speechSeconds{0.0};
  VoiceProfileSource source{VoiceProfileSource::Passive};
  int64_t linkedAt{0};
  int64_t refreshedAt{0};
  int64_t createdAt{0};

  VoiceProfileSchema() = default;
  explicit VoiceProfileSchema(const drogon::orm::Row& row);
};
