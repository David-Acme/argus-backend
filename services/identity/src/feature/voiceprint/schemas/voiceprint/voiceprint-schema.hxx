#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <feature/voiceprint/vocabulary/voiceprint-method.hxx>
#include <optional>
#include <string>
#include <vector>

struct VoiceprintSchema
{
  int64_t id{0};
  int64_t userId{0};
  std::string model;
  std::vector<float> embedding;
  int sampleCount{0};
  double speechSeconds{0.0};
  VoiceprintMethod method{VoiceprintMethod::Self};
  std::string consentVersion;
  std::optional<int64_t> enrolledBy;
  int64_t createdAt{0};

  VoiceprintSchema() = default;
  explicit VoiceprintSchema(const drogon::orm::Row& row);
};
