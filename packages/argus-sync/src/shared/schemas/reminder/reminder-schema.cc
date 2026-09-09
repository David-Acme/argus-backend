#include "reminder-schema.hxx"

ReminderSchema::ReminderSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  if (!row["created_by"].isNull())
    createdBy = static_cast<int64_t>(row["created_by"].as<long long>());
  targetUserId = static_cast<int64_t>(row["target_user_id"].as<long long>());
  title = row["title"].as<std::string>();
  description = row["description"].as<std::string>();
  scheduledAt = static_cast<int64_t>(row["scheduled_at"].as<long long>());
  if (!row["recurrence_rule"].isNull())
    recurrenceRule = row["recurrence_rule"].as<std::string>();
  isCompleted = row["is_completed"].as<int>() != 0;
  if (!row["completed_at"].isNull())
    completedAt = static_cast<int64_t>(row["completed_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value ReminderSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["createdBy"] = createdBy ? Json::Value(Json::Int64(*createdBy)) : Json::Value();
  json["targetUserId"] = targetUserId;
  json["title"] = title;
  json["description"] = description;
  json["scheduledAt"] = Json::Int64(scheduledAt);
  json["recurrenceRule"] =
      recurrenceRule ? Json::Value(*recurrenceRule) : Json::nullValue;
  json["isCompleted"] = isCompleted;
  json["completedAt"] =
      completedAt ? Json::Value(Json::Int64(*completedAt)) : Json::Value();
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt)) : Json::Value();
  json["deletedAt"] = deletedAt ? Json::Value(Json::Int64(*deletedAt)) : Json::Value();
  return json;
}
