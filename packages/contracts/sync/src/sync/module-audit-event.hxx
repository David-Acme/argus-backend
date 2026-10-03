#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <json/value.h>
#include <optional>
#include <sync/audit-log-priority.hxx>
#include <sync/sync-change.hxx>
#include <sync/table-name.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>

struct ModuleAuditInput
{
  int64_t recordId{0};
  TableName tableName{TableName::Camera};
  Json::Value before;
  Json::Value after;
  std::optional<int64_t> actorId;
  drogon::orm::DbClient* client{nullptr};
};

struct ModuleAuditEvent
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
    json[sync_change::kKindField] = sync_change::kKindAudit;
    json["record_id"] = recordId;
    json["table_name"] = tableNameToString(tableName);
    json["changes"] = JsonDiff::toJson(changes);
    json["priority"] = static_cast<int>(priority);
    if (createUserId)
      json["create_user_id"] = static_cast<Json::Int64>(*createUserId);
    json["event_timestamp"] = eventTimestamp;
    return json;
  }

  static std::optional<ModuleAuditEvent> fromJson(const Json::Value& json)
  {
    if (!json.isObject() || !json.isMember("record_id") ||
        !json["record_id"].isInt64() || !json.isMember("table_name") ||
        !json["table_name"].isString() || !json.isMember("changes") ||
        !json.isMember("event_timestamp") ||
        !json["event_timestamp"].isInt64())
      return std::nullopt;

    ModuleAuditEvent event;
    event.recordId = json["record_id"].asInt64();
    const auto tableName = findTableName(json["table_name"].asString());
    if (!tableName)
      return std::nullopt;
    event.tableName = *tableName;
    event.changes =
        JsonDiff::fromJsonString(json_util::toString(json["changes"]));
    event.eventTimestamp = json["event_timestamp"].asInt64();
    if (json.isMember("priority") && json["priority"].isInt()) {
      const auto priority = auditLogPriorityFromInt(json["priority"].asInt());
      if (!priority)
        return std::nullopt;
      event.priority = *priority;
    }
    if (json.isMember("create_user_id") && json["create_user_id"].isInt64())
      event.createUserId = json["create_user_id"].asInt64();
    return event;
  }
};
