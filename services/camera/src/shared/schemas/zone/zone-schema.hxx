#pragma once

#include <cstdint>
#include <ctime>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>
#include <sync/socket-emit-dto.hxx>
#include <camera/zone-type.hxx>
#include <utility>
#include <vector>

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

std::vector<std::pair<double, double>> parseZonePoints(const std::string& text);

inline SocketEmitDto zoneBody(SyncOperation operation, const ZoneSchema& row)
{
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::Zone;
  if (operation == SyncOperation::Delete) {
    Json::Value tombstone;
    tombstone["id"] = row.id;
    tombstone["deletedAt"] = row.deletedAt.value_or(std::time(nullptr));
    body.obj = std::move(tombstone);
  }
  else {
    body.obj = row.toJson();
  }
  return body;
}
