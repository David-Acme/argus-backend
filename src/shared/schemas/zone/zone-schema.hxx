#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <string>

struct ZoneSchema
{
  int64_t id{0};
  int64_t cameraId{0};
  std::string name;
  std::string points;
  ZoneType zoneType{ZoneType::Monitor};
  std::string color;
  bool isEnabled{true};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  ZoneSchema() = default;
  explicit ZoneSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
