#pragma once

#include <json/value.h>
#include <optional>
#include <string>
#include <validation/validation_dsl.hxx>
#include <voice/voice-lang.hxx>

struct CreateVoiceprintChallengeDto
{
  std::optional<std::string> lang;

  [[nodiscard]] std::optional<VoiceLang> language() const;

  static CreateVoiceprintChallengeDto fromJson(const Json::Value& json);
};
