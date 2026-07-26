#include "audit-log-schema.hxx"

AuditLogSchema::AuditLogSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  recordId = static_cast<int64_t>(row["record_id"].as<long long>());
  tableName = row["table_name"].as<std::string>();
  eventTimestamp = static_cast<int64_t>(row["event_timestamp"].as<long long>());
  oldData = row["old_data"].as<std::string>();
  newData = row["new_data"].as<std::string>();
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
}

Json::Value AuditLogSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["recordId"] = recordId;
  json["tableName"] = tableName;
  json["eventTimestamp"] = Json::Int64(eventTimestamp);
  json["oldData"] = oldData;
  json["newData"] = newData;
  json["createdAt"] = Json::Int64(createdAt);
  return json;
}
