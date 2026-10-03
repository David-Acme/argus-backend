#pragma once

#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>
#include <json/value.h>
#include <string>

struct ResponseVoiceprintChallengeDto
{
  VoiceprintChallengeView challenge;
  std::string consentVersion;
  int samplesRequired{0};
  float minSpeechSeconds{0.0F};

  [[nodiscard]] Json::Value toJson() const;
};
