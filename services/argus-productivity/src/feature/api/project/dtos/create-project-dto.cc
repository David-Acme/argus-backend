#include "create-project-dto.hxx"

CreateProjectDto CreateProjectDto::fromJson(const Json::Value& json)
{
  CreateProjectDto dto;
  dto.name = json.get("name", "").asString();
  dto.description = json.get("description", "").asString();
  dto.status = json.get("status", "active").asString();
  dto.color = json.get("color", "").asString();
  if (json.isMember("startsAt") && json["startsAt"].isInt64())
    dto.startsAt = json["startsAt"].asInt64();
  if (json.isMember("targetAt") && json["targetAt"].isInt64())
    dto.targetAt = json["targetAt"].asInt64();

  START_VALIDATION(CreateProjectDto, dto)
  IS_NOT_EMPTY(name)
  MAX_LENGTH(name, 160)
  MAX_LENGTH(description, 2000)
  IS_IN(status, "planned", "active", "paused", "done", "canceled")
  IS_POSITIVE_TIMESTAMP_OPTIONAL(startsAt)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(targetAt)
  END_VALIDATION()
  return dto;
}
