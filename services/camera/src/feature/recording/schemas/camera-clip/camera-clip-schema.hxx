#pragma once

#include <camera/camera-clip-trigger.hxx>
#include <cstdint>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct CameraClipSchema
{
  int64_t id{0};
  int64_t cameraId{0};
  CameraClipTrigger trigger{CameraClipTrigger::Event};
  std::string objectKey;
  std::string contentType{"video/mp4"};
  std::string codec;
  int64_t startedAt{0};
  int64_t durationMs{0};
  int64_t byteSize{0};
  int64_t createdAt{0};
  int64_t expiresAt{0};
  std::optional<int64_t> deletedAt;

  CameraClipSchema() = default;
  explicit CameraClipSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
