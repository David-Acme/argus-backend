#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

enum class CameraRole : uint8_t
{
  Other = 0,
  Entrance,
  Perimeter,
  Garage,
  Living,
  Kitchen,
  Office,
  Register,
  Storage,
  PublicArea
};

inline constexpr std::array<std::pair<CameraRole, std::string_view>, 10>
    kCameraRoleNames{{{CameraRole::Other, "other"},
                      {CameraRole::Entrance, "entrance"},
                      {CameraRole::Perimeter, "perimeter"},
                      {CameraRole::Garage, "garage"},
                      {CameraRole::Living, "living"},
                      {CameraRole::Kitchen, "kitchen"},
                      {CameraRole::Office, "office"},
                      {CameraRole::Register, "register"},
                      {CameraRole::Storage, "storage"},
                      {CameraRole::PublicArea, "public_area"}}};

inline std::string cameraRoleToString(CameraRole role)
{
  for (const auto& [value, name] : kCameraRoleNames) {
    if (value == role)
      return std::string(name);
  }
  return "other";
}

inline std::optional<CameraRole> cameraRoleFromString(std::string_view value)
{
  for (const auto& [role, name] : kCameraRoleNames) {
    if (name == value)
      return role;
  }
  return std::nullopt;
}

inline bool cameraRoleIsWorkArea(CameraRole role)
{
  return role == CameraRole::Kitchen || role == CameraRole::Office ||
         role == CameraRole::Register || role == CameraRole::Storage;
}
