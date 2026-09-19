#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <shared/utils/json-diff/json-diff.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <string>

// Camera-domain audit row on argus.camera.v1.change; the gateway inserts it verbatim.
struct CameraAuditEvent
{
  int64_t recordId{0};
  TableName tableName{TableName::Camera};
  ChangesDiff changes;
  AuditLogPriority priority{AuditLogPriority::Medium};
  std::optional<int64_t> createUserId;
  int64_t eventTimestamp{0};

  Json::Value toJson() const
  {
    Json::Value json;
    json["kind"] = "audit";
    json["record_id"] = recordId;
    json["table_name"] = tableNameToString(tableName);
    json["changes"] = JsonDiff::toJson(changes);
    json["priority"] = static_cast<int>(priority);
    if (createUserId)
      json["create_user_id"] = *createUserId;
    json["event_timestamp"] = eventTimestamp;
    return json;
  }

  static std::optional<CameraAuditEvent> fromJson(const Json::Value& json)
  {
    if (!json.isObject() || !json.isMember("record_id") ||
        !json["record_id"].isInt64() || !json.isMember("table_name") ||
        !json["table_name"].isString() || !json.isMember("changes") ||
        !json.isMember("event_timestamp"))
      return std::nullopt;

    CameraAuditEvent event;
    event.recordId = json["record_id"].asInt64();
    event.tableName = tableNameFromString(json["table_name"].asString());
    event.changes = JsonDiff::fromJsonString(json_util::toString(json["changes"]));
    event.eventTimestamp = json["event_timestamp"].asInt64();
    if (json.isMember("priority") && json["priority"].isInt())
      event.priority = static_cast<AuditLogPriority>(json["priority"].asInt());
    if (json.isMember("create_user_id") && json["create_user_id"].isInt64())
      event.createUserId = json["create_user_id"].asInt64();
    return event;
  }
};
