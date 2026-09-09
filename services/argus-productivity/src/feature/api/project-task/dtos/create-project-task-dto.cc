#include "create-project-task-dto.hxx"

CreateProjectTaskDto CreateProjectTaskDto::fromJson(const Json::Value& json)
{
  CreateProjectTaskDto dto;
  if (json.isMember("projectId") && json["projectId"].isInt64())
    dto.projectId = json["projectId"].asInt64();
  dto.title = json.get("title", "").asString();
  dto.status = json.get("status", "todo").asString();
  dto.priority = json.get("priority", "none").asString();
  if (json.isMember("assigneeId") && json["assigneeId"].isInt64())
    dto.assigneeId = json["assigneeId"].asInt64();
  if (json.isMember("dueAt") && json["dueAt"].isInt64())
    dto.dueAt = json["dueAt"].asInt64();
  if (json.isMember("sortOrder") && json["sortOrder"].isNumeric())
    dto.sortOrder = json["sortOrder"].asDouble();

  START_VALIDATION(CreateProjectTaskDto, dto)
  IS_POSITIVE(projectId)
  IS_NOT_EMPTY(title)
  MAX_LENGTH(title, 200)
  IS_IN(status, "backlog", "todo", "doing", "done", "canceled")
  IS_IN(priority, "none", "low", "medium", "high", "urgent")
  IS_POSITIVE_TIMESTAMP_OPTIONAL(dueAt)
  END_VALIDATION()
  return dto;
}
