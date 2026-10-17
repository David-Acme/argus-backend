#pragma once

#include <cstdint>
#include <feature/recording/schemas/camera-clip/camera-clip-schema.hxx>
#include <json/value.h>
#include <vector>

struct ResponseClipsDto
{
  int64_t cameraId{0};
  std::vector<CameraClipSchema> clips;

  Json::Value toJson() const;
};
