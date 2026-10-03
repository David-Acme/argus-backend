#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <sync/audit-log-priority.hxx>
#include <sync/sync-change.hxx>
#include <sync/table-name.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>
#include <vector>

struct UserAuditEvent
{
  int64_t recordId{0};
  TableName tableName{TableName::Notification};
  ChangesDiff changes;
  AuditLogPriority priority{AuditLogPriority::Medium};
  std::vector<int64_t> users;
  int64_t eventTimestamp{0};

  Json::Value toJson() const
  {
    Json::Value json;
    json[sync_change::kKindField] = sync_change::kKindAudit;
    json["record_id"] = recordId;
    json["table_name"] = tableNameToString(tableName);
    json["changes"] = JsonDiff::toJson(changes);
    json["priority"] = static_cast<int>(priority);
    Json::Value recipients(Json::arrayValue);
    for (const auto userId : users)
      recipients.append(static_cast<Json::Int64>(userId));
    json["users"] = recipients;
    json["event_timestamp"] = eventTimestamp;
    return json;
  }

  static std::optional<UserAuditEvent> fromJson(const Json::Value& json)
  {
    if (!json.isObject() || !json.isMember("record_id") ||
        !json["record_id"].isInt64() || !json.isMember("table_name") ||
        !json["table_name"].isString() || !json.isMember("changes") ||
        !json.isMember("users") || !json["users"].isArray() ||
        !json.isMember("event_timestamp") ||
        !json["event_timestamp"].isInt64())
      return std::nullopt;

    UserAuditEvent event;
    event.recordId = json["record_id"].asInt64();
    const auto tableName = findTableName(json["table_name"].asString());
    if (!tableName)
      return std::nullopt;
    event.tableName = *tableName;
    event.changes = JsonDiff::fromJsonString(json_util::toString(json["changes"]));
    for (const auto& userId : json["users"]) {
      if (!userId.isInt64())
        return std::nullopt;
      event.users.push_back(userId.asInt64());
    }
    event.eventTimestamp = json["event_timestamp"].asInt64();
    if (json.isMember("priority") && json["priority"].isInt()) {
      const auto priority = auditLogPriorityFromInt(json["priority"].asInt());
      if (!priority)
        return std::nullopt;
      event.priority = *priority;
    }
    return event;
  }
};
