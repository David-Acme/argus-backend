#include "event-schema.hxx"

EventSchema::EventSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  eventType = row["event_type"].as<std::string>();
  severity = eventSeverityFromString(row["severity"].as<std::string>());
  source = row["source"].as<std::string>();
  summary = row["summary"].as<std::string>();
  details = row["details"].as<std::string>();
  occurredAt = static_cast<int64_t>(row["occurred_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value EventSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["eventType"] = eventType;
  json["severity"] = eventSeverityToString(severity);
  json["source"] = source;
  json["summary"] = summary;
  json["details"] = details;
  json["occurredAt"] = Json::Int64(occurredAt);
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt)) : Json::Value();
  json["deletedAt"] = deletedAt ? Json::Value(Json::Int64(*deletedAt)) : Json::Value();
  return json;
}
