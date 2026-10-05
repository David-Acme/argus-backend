#pragma once

#include <json/value.h>
#include <shared/vocabulary/camera-stream-role.hxx>

#include <optional>
#include <string>

struct CameraWebRtcOfferDto
{
  std::string sdp;
  std::optional<std::string> quality;

  static CameraWebRtcOfferDto fromJson(const Json::Value& json);

  [[nodiscard]] CameraStream stream() const;
};
