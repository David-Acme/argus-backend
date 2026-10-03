#pragma once

#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <json/value.h>

struct ResponseVoiceprintVerifyDto
{
  VoiceprintVerifyResult result;

  [[nodiscard]] Json::Value toJson() const;
};
