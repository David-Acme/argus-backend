#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class ConversationSurface : uint8_t
{
  App = 0,
  Camera,
  CameraVisitor,
  CameraMinor
};

[[nodiscard]] inline std::string_view conversationSurfaceToString(ConversationSurface surface)
{
  switch (surface) {
    case ConversationSurface::App:
      return "app";
    case ConversationSurface::Camera:
      return "camera";
    case ConversationSurface::CameraVisitor:
      return "camera_visitor";
    case ConversationSurface::CameraMinor:
      return "camera_minor";
  }
  return "app";
}

[[nodiscard]] inline bool cameraSurface(ConversationSurface surface)
{
  return surface != ConversationSurface::App;
}

[[nodiscard]] inline std::optional<ConversationSurface> conversationSurfaceFromString(std::string_view value)
{
  if (value == "app")
    return ConversationSurface::App;
  if (value == "camera")
    return ConversationSurface::Camera;
  if (value == "camera_visitor")
    return ConversationSurface::CameraVisitor;
  if (value == "camera_minor")
    return ConversationSurface::CameraMinor;
  return std::nullopt;
}
