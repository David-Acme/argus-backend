#include "reminder-detail-schema.hxx"

ReminderDetailSchema::ReminderDetailSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  reminderId = static_cast<int64_t>(row["reminder_id"].as<long long>());
  if (!row["created_by"].isNull())
    createdBy = static_cast<int64_t>(row["created_by"].as<long long>());
  content = row["content"].as<std::string>();
  status =
      reminderDetailStatusFromString(row["status"].as<std::string>());
  filePaths = row["file_paths"].as<std::string>();
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value ReminderDetailSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["reminderId"] = reminderId;
  json["createdBy"] =
      createdBy ? Json::Int64(*createdBy) : Json::nullValue;
  json["content"] = content;
  json["status"] = reminderDetailStatusToString(status);
  json["filePaths"] = filePaths;
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Int64(*updatedAt) : Json::nullValue;
  json["deletedAt"] = deletedAt ? Json::Int64(*deletedAt) : Json::nullValue;
  return json;
}
