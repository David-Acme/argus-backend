#pragma once

#include <cstdint>
#include <string>

enum class CameraClipTrigger : uint8_t
{
  Motion = 0,
  Event,
  Continuous
};

inline std::string cameraClipTriggerToString(CameraClipTrigger trigger)
{
  switch (trigger) {
    case CameraClipTrigger::Motion:
      return "motion";
    case CameraClipTrigger::Continuous:
      return "continuous";
    case CameraClipTrigger::Event:
      return "event";
  }
  return "event";
}

inline CameraClipTrigger cameraClipTriggerFromString(const std::string& s)
{
  if (s == "motion")
    return CameraClipTrigger::Motion;
  if (s == "continuous")
    return CameraClipTrigger::Continuous;
  return CameraClipTrigger::Event;
}
