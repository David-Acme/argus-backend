#pragma once

#include <cstdint>
#include <string>

enum class CameraRecordMode : uint8_t
{
  Events = 0,
  Continuous
};

inline std::string cameraRecordModeToString(CameraRecordMode m)
{
  return m == CameraRecordMode::Continuous ? "continuous" : "events";
}

inline CameraRecordMode cameraRecordModeFromString(const std::string& s)
{
  if (s == "continuous")
    return CameraRecordMode::Continuous;
  return CameraRecordMode::Events;
}
