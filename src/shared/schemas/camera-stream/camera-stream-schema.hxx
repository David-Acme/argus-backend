#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct CameraStreamSchema
{
  int64_t id{0};
  int64_t cameraId{0};
  std::string label;
  std::string url;
  std::string resolution;
  int32_t fps{0};
  std::string codec;
  bool isPrimary{true};
  bool isEnabled{true};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  CameraStreamSchema() = default;
  explicit CameraStreamSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
