#pragma once

#include <cstdint>
#include <string>

enum class CameraClipTrigger : uint8_t
{
  Motion = 0,
  Event,
  Continuous,
  Tamper,
  Look
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
    case CameraClipTrigger::Tamper:
      return "tamper";
    case CameraClipTrigger::Look:
      return "look";
  }
  return "event";
}

inline CameraClipTrigger cameraClipTriggerFromString(const std::string& s)
{
  if (s == "motion")
    return CameraClipTrigger::Motion;
  if (s == "continuous")
    return CameraClipTrigger::Continuous;
  if (s == "tamper")
    return CameraClipTrigger::Tamper;
  if (s == "look")
    return CameraClipTrigger::Look;
  return CameraClipTrigger::Event;
}
