#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <shared/utils/json-diff/json-diff.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <vector>

// User-scoped audit row on the productivity/notification subjects; the gateway inserts it verbatim.
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
    json["kind"] = "audit";
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
    event.tableName = tableNameFromString(json["table_name"].asString());
    event.changes = JsonDiff::fromJsonString(json_util::toString(json["changes"]));
    for (const auto& userId : json["users"])
      event.users.push_back(userId.asInt64());
    event.eventTimestamp = json["event_timestamp"].asInt64();
    if (json.isMember("priority") && json["priority"].isInt())
      event.priority = static_cast<AuditLogPriority>(json["priority"].asInt());
    return event;
  }
};
