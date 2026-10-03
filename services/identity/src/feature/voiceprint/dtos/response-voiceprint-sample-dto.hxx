#pragma once

#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <json/value.h>

struct ResponseVoiceprintSampleDto
{
  VoiceprintSampleCheck check;

  [[nodiscard]] Json::Value toJson() const;
};
