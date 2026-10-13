#pragma once

#include <drogon/HttpRequest.h>
#include <shared/vocabulary/camera-stream-role.hxx>

#include <optional>
#include <string>

struct CameraSnapshotDto
{
  std::optional<std::string> quality;

  static CameraSnapshotDto fromRequest(const drogon::HttpRequestPtr& request);

  [[nodiscard]] CameraStream stream() const;
};
