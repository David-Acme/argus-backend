#pragma once

#include <json/value.h>
#include <optional>
#include <validation/validation_dsl.hxx>

struct UpdateHouseholdPrivacyDto
{
  std::optional<bool> presence;
  std::optional<bool> faceCameras;
  std::optional<bool> voiceLearning;
  std::optional<bool> cameraAudio;
  std::optional<bool> visitorRecognition;
  bool acknowledge{false};

  static UpdateHouseholdPrivacyDto fromJson(const Json::Value& json);
};
