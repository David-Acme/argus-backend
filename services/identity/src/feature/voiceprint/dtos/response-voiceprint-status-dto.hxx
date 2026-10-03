#pragma once

#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <json/value.h>

struct ResponseVoiceprintStatusDto
{
  VoiceprintStatusView status;

  [[nodiscard]] Json::Value toJson() const;
};
