#pragma once

#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>

/** Device-side switches. Absent fields are left as the camera has them. */
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

  static CameraSettingsDto fromJson(const Json::Value& json);
};
