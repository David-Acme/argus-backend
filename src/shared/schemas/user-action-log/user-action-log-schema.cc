#include "user-action-log-schema.hxx"

#include <shared/utils/json-util/json-util.hxx>

UserActionLogSchema::UserActionLogSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  userId = static_cast<int64_t>(row["user_id"].as<long long>());
  recordId = static_cast<int64_t>(row["record_id"].as<long long>());
  tableName = tableNameFromString(row["table_name"].as<std::string>());
  action = userActionFromString(row["action"].as<std::string>());
  oldData = json_util::fromString(row["old_data"].as<std::string>());
  newData = json_util::fromString(row["new_data"].as<std::string>());
  ipAddress = row["ip_address"].as<std::string>();
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
}

Json::Value UserActionLogSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["userId"] = userId;
  json["recordId"] = recordId;
  json["tableName"] = tableNameToString(tableName);
  json["action"] = userActionToString(action);
  json["oldData"] = oldData;
  json["newData"] = newData;
  json["ipAddress"] = ipAddress;
  json["createdAt"] = Json::Int64(createdAt);
  return json;
}
