#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class CameraStream : uint8_t
{
  Main = 0,
  Sub
};

enum class CameraStreamRole : uint8_t
{
  LiveView = 0,
  Analysis,
  Listening
};

namespace camera_stream_role
{
inline constexpr CameraStream streamFor(CameraStreamRole role)
{
  switch (role) {
    case CameraStreamRole::LiveView:
      return CameraStream::Main;
    case CameraStreamRole::Analysis:
    case CameraStreamRole::Listening:
      return CameraStream::Sub;
  }
  return CameraStream::Sub;
}

inline constexpr bool isWarm(CameraStream stream)
{
  return stream == streamFor(CameraStreamRole::Analysis);
}
}

inline std::string cameraStreamToString(CameraStream stream)
{
  return stream == CameraStream::Sub ? "sub" : "main";
}

inline std::optional<CameraStream> cameraStreamFromString(std::string_view value)
{
  if (value == "main")
    return CameraStream::Main;
  if (value == "sub")
    return CameraStream::Sub;
  return std::nullopt;
}
