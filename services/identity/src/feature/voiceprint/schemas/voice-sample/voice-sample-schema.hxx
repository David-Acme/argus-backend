#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <feature/voiceprint/vocabulary/voice-sample-state.hxx>
#include <string>
#include <vector>

struct VoiceSampleSchema
{
  int64_t id{0};
  int64_t userId{0};
  std::string model;
  std::string deviceHash;
  std::vector<float> embedding;
  int turns{0};
  double speechSeconds{0.0};
  VoiceSampleState state{VoiceSampleState::Pending};
  int64_t createdAt{0};

  VoiceSampleSchema() = default;
  explicit VoiceSampleSchema(const drogon::orm::Row& row);
};
