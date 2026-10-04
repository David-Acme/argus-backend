#pragma once

#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <json/value.h>

struct ResponseVoiceprintDirectoryDto
{
  VoiceprintDirectory directory;

  [[nodiscard]] Json::Value toJson() const;
};
