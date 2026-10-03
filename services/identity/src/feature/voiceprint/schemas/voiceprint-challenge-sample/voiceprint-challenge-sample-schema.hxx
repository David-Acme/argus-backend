#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <vector>

struct VoiceprintChallengeSampleSchema
{
  int64_t challengeId{0};
  int position{0};
  std::vector<float> embedding;
  double speechSeconds{0.0};
  int64_t createdAt{0};

  VoiceprintChallengeSampleSchema() = default;
  explicit VoiceprintChallengeSampleSchema(const drogon::orm::Row& row);
};
