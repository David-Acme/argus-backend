#include "calendar-event-share-schema.hxx"

CalendarEventShareSchema::CalendarEventShareSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  calendarEventId = static_cast<int64_t>(row["calendar_event_id"].as<long long>());
  userId = static_cast<int64_t>(row["user_id"].as<long long>());
  access = row["access"].as<std::string>();
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value CalendarEventShareSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["calendarEventId"] = calendarEventId;
  json["userId"] = userId;
  json["access"] = access;
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt)) : Json::Value();
  json["deletedAt"] = deletedAt ? Json::Value(Json::Int64(*deletedAt)) : Json::Value();
  return json;
}
