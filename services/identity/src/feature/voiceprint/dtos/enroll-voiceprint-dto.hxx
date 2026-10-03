#pragma once

#include <json/value.h>
#include <string>
#include <validation/validation_dsl.hxx>

struct EnrollVoiceprintDto
{
  bool consent{false};
  std::string consentVersion;
  std::string challengeId;
  std::string face;

  [[nodiscard]] std::string faceImage() const;

  static EnrollVoiceprintDto fromJson(const Json::Value& json);
};
