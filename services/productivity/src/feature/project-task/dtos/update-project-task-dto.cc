#include "update-project-task-dto.hxx"

UpdateProjectTaskDto UpdateProjectTaskDto::fromJson(const Json::Value& json)
{
  UpdateProjectTaskDto dto;
  if (json.isMember("title") && json["title"].isString())
    dto.title = json["title"].asString();
  if (json.isMember("status") && json["status"].isString())
    dto.status = json["status"].asString();
  if (json.isMember("priority") && json["priority"].isString())
    dto.priority = json["priority"].asString();
  if (json.isMember("assigneeId") && json["assigneeId"].isInt64())
    dto.assigneeId = json["assigneeId"].asInt64();
  if (json.isMember("dueAt") && json["dueAt"].isInt64())
    dto.dueAt = json["dueAt"].asInt64();
  if (json.isMember("sortOrder") && json["sortOrder"].isNumeric())
    dto.sortOrder = json["sortOrder"].asDouble();

  START_VALIDATION(UpdateProjectTaskDto, dto)
  IS_NOT_EMPTY_OPTIONAL(title)
  MAX_LENGTH_OPTIONAL(title, 200)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(dueAt)
  END_VALIDATION()
  return dto;
}
