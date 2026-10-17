#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class CameraPrivacyState : std::uint8_t
{
  Unknown = 0,
  On,
  Off
};

inline std::string cameraPrivacyStateToString(CameraPrivacyState state)
{
  switch (state)
  {
    case CameraPrivacyState::On:
      return "on";
    case CameraPrivacyState::Off:
      return "off";
    case CameraPrivacyState::Unknown:
      return "unknown";
  }
  return "unknown";
}

inline std::optional<CameraPrivacyState> cameraPrivacyStateFromString(
    std::string_view name)
{
  if (name == "on")
    return CameraPrivacyState::On;
  if (name == "off")
    return CameraPrivacyState::Off;
  if (name == "unknown")
    return CameraPrivacyState::Unknown;
  return std::nullopt;
}

inline bool cameraPrivacyMasked(CameraPrivacyState state)
{
  return state == CameraPrivacyState::On;
}
