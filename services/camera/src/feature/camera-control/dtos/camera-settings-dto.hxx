#pragma once

#include <json/value.h>
#include <optional>
#include <validation/validation_dsl.hxx>
#include <string>

struct CameraSettingsDto
{
  std::optional<bool> privacy;
  std::optional<bool> led;
  std::optional<std::string> dayNight;
  std::optional<bool> motion;
  std::optional<int> motionSensitivity;
  std::optional<bool> autoTrack;
  std::optional<bool> alarm;
  std::optional<int> alarmVolume;
  std::optional<int> frameRate;

  static CameraSettingsDto fromJson(const Json::Value& json);
};
