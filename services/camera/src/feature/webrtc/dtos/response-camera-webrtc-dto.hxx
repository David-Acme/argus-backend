#pragma once

#include <json/value.h>
#include <shared/vocabulary/camera-stream-role.hxx>

#include <string>

struct ResponseCameraWebRtcDto
{
  std::string sdp;
  CameraStream stream{CameraStream::Main};
  bool audio{false};

  [[nodiscard]] Json::Value toJson() const;
};
