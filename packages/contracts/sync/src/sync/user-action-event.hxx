#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>
#include <sync/table-name.hxx>
#include <sync/user-action.hxx>
#include <text/json-util.hxx>

struct UserActionEvent
{
  int64_t userId{0};
  int64_t recordId{0};
  TableName tableName{TableName::User};
  UserAction action{UserAction::Create};
  Json::Value oldData;
  Json::Value newData;
  std::string ipAddress;

  [[nodiscard]] Json::Value toJson() const
  {
    Json::Value json;
    json["user_id"] = static_cast<Json::Int64>(userId);
    json["record_id"] = static_cast<Json::Int64>(recordId);
    json["table_name"] = tableNameToString(tableName);
    json["action"] = userActionToString(action);
    json["old_data"] = oldData;
    json["new_data"] = newData;
    json["ip_address"] = ipAddress;
    return json;
  }

  static std::optional<UserActionEvent> fromJson(const Json::Value& json)
  {
    if (!json.isObject() || !json.isMember("user_id") ||
        !json["user_id"].isInt64() || !json.isMember("record_id") ||
        !json["record_id"].isInt64() || !json.isMember("table_name") ||
        !json["table_name"].isString() || !json.isMember("action") ||
        !json["action"].isString())
      return std::nullopt;

    UserActionEvent event;
    event.userId = json["user_id"].asInt64();
    event.recordId = json["record_id"].asInt64();
    event.tableName = tableNameFromString(json["table_name"].asString());
    event.action = userActionFromString(json["action"].asString());
    if (json.isMember("old_data"))
      event.oldData = json_util::fromString(json_util::toString(json["old_data"]));
    if (json.isMember("new_data"))
      event.newData = json_util::fromString(json_util::toString(json["new_data"]));
    if (json.isMember("ip_address") && json["ip_address"].isString())
      event.ipAddress = json["ip_address"].asString();
    return event;
  }
};
