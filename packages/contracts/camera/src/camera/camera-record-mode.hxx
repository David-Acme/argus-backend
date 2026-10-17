#pragma once

#include <cstdint>
#include <string>

enum class CameraRecordMode : uint8_t
{
  Off = 0,
  Motion,
  Continuous,
  Events
};

inline std::string cameraRecordModeToString(CameraRecordMode m)
{
  switch (m) {
    case CameraRecordMode::Off:
      return "off";
    case CameraRecordMode::Motion:
      return "motion";
    case CameraRecordMode::Continuous:
      return "continuous";
    case CameraRecordMode::Events:
      return "events";
  }
  return "events";
}

inline CameraRecordMode cameraRecordModeFromString(const std::string& s)
{
  if (s == "off")
    return CameraRecordMode::Off;
  if (s == "motion")
    return CameraRecordMode::Motion;
  if (s == "continuous")
    return CameraRecordMode::Continuous;
  return CameraRecordMode::Events;
}
