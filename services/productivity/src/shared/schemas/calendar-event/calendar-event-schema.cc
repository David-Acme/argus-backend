#include "calendar-event-schema.hxx"

CalendarEventSchema::CalendarEventSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  if (!row["created_by"].isNull())
    createdBy = static_cast<int64_t>(row["created_by"].as<long long>());
  ownerId = static_cast<int64_t>(row["owner_id"].as<long long>());
  if (!row["project_id"].isNull())
    projectId = static_cast<int64_t>(row["project_id"].as<long long>());
  title = row["title"].as<std::string>();
  description = row["description"].as<std::string>();
  location = row["location"].as<std::string>();
  color = row["color"].as<std::string>();
  startsAt = static_cast<int64_t>(row["starts_at"].as<long long>());
  if (!row["ends_at"].isNull())
    endsAt = static_cast<int64_t>(row["ends_at"].as<long long>());
  isAllDay = row["is_all_day"].as<int>() != 0;
  if (!row["recurrence_rule"].isNull())
    recurrenceRule = row["recurrence_rule"].as<std::string>();
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value CalendarEventSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["createdBy"] = createdBy ? Json::Value(Json::Int64(*createdBy)) : Json::Value();
  json["ownerId"] = ownerId;
  json["projectId"] = projectId ? Json::Value(Json::Int64(*projectId)) : Json::Value();
  json["title"] = title;
  json["description"] = description;
  json["location"] = location;
  json["color"] = color;
  json["startsAt"] = Json::Int64(startsAt);
  json["endsAt"] = endsAt ? Json::Value(Json::Int64(*endsAt)) : Json::Value();
  json["isAllDay"] = isAllDay;
  json["recurrenceRule"] =
      recurrenceRule ? Json::Value(*recurrenceRule) : Json::nullValue;
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt)) : Json::Value();
  json["deletedAt"] = deletedAt ? Json::Value(Json::Int64(*deletedAt)) : Json::Value();
  return json;
}
