#include "project-task-schema.hxx"

ProjectTaskSchema::ProjectTaskSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  projectId = static_cast<int64_t>(row["project_id"].as<long long>());
  if (!row["created_by"].isNull())
    createdBy = static_cast<int64_t>(row["created_by"].as<long long>());
  if (!row["assignee_id"].isNull())
    assigneeId = static_cast<int64_t>(row["assignee_id"].as<long long>());
  title = row["title"].as<std::string>();
  status = row["status"].as<std::string>();
  priority = row["priority"].as<std::string>();
  if (!row["due_at"].isNull())
    dueAt = static_cast<int64_t>(row["due_at"].as<long long>());
  sortOrder = row["sort_order"].as<double>();
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  if (!row["updated_at"].isNull())
    updatedAt = static_cast<int64_t>(row["updated_at"].as<long long>());
  if (!row["deleted_at"].isNull())
    deletedAt = static_cast<int64_t>(row["deleted_at"].as<long long>());
}

Json::Value ProjectTaskSchema::toJson() const
{
  Json::Value json;
  json["id"] = id;
  json["projectId"] = projectId;
  json["createdBy"] = createdBy ? Json::Value(Json::Int64(*createdBy)) : Json::Value();
  json["assigneeId"] = assigneeId ? Json::Value(Json::Int64(*assigneeId)) : Json::Value();
  json["title"] = title;
  json["status"] = status;
  json["priority"] = priority;
  json["dueAt"] = dueAt ? Json::Value(Json::Int64(*dueAt)) : Json::Value();
  json["sortOrder"] = sortOrder;
  json["createdAt"] = Json::Int64(createdAt);
  json["updatedAt"] = updatedAt ? Json::Value(Json::Int64(*updatedAt)) : Json::Value();
  json["deletedAt"] = deletedAt ? Json::Value(Json::Int64(*deletedAt)) : Json::Value();
  return json;
}
