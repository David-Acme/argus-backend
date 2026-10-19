#pragma once

#include <feature/camera-control/schemas/camera-patrol/camera-patrol-schema.hxx>

#include <json/value.h>
#include <optional>

struct ResponseCameraPatrolDto
{
  std::optional<CameraPatrolSchema> patrol;

  [[nodiscard]] Json::Value toJson() const;
};
