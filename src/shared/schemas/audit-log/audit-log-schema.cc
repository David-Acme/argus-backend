#include "audit-log-schema.hxx"

#include <shared/utils/json-util/json-util.hxx>

AuditLogSchema::AuditLogSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  if (!row["create_user_id"].isNull())
    createUserId = static_cast<int64_t>(row["create_user_id"].as<long long>());
  recordId = static_cast<int64_t>(row["record_id"].as<long long>());
  tableName = tableNameFromString(row["table_name"].as<std::string>());
  changes = json_util::fromString(row["changes"].as<std::string>());
  priority = static_cast<AuditLogPriority>(row["priority"].as<int>());
  eventTimestamp = static_cast<int64_t>(row["event_timestamp"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
}

Json::Value AuditLogSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["createUserId"] = createUserId ? Json::Value(Json::Int64(*createUserId))
                                      : Json::Value();
  json["recordId"] = recordId;
  json["tableName"] = tableNameToString(tableName);
  json["changes"] = changes;
  json["priority"] = static_cast<int>(priority);
  json["eventTimestamp"] = Json::Int64(eventTimestamp);
  json["createdAt"] = Json::Int64(createdAt);
  return json;
}
