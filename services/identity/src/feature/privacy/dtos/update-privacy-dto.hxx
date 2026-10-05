#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <validation/validation_dsl.hxx>

struct UpdatePrivacyDto
{
  int64_t noticeVersion{0};
  std::optional<bool> presence;
  std::optional<bool> faceCameras;
  std::optional<bool> voiceLearning;
  std::optional<bool> cameraAudio;

  static UpdatePrivacyDto fromJson(const Json::Value& json);
};
