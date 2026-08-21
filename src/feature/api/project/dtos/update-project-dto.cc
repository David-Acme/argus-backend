#include "update-project-dto.hxx"

UpdateProjectDto UpdateProjectDto::fromJson(const Json::Value& json)
{
  UpdateProjectDto dto;
  if (json.isMember("name") && json["name"].isString())
    dto.name = json["name"].asString();
  if (json.isMember("description") && json["description"].isString())
    dto.description = json["description"].asString();
  if (json.isMember("status") && json["status"].isString())
    dto.status = json["status"].asString();
  if (json.isMember("color") && json["color"].isString())
    dto.color = json["color"].asString();
  if (json.isMember("startsAt") && json["startsAt"].isInt64())
    dto.startsAt = json["startsAt"].asInt64();
  if (json.isMember("targetAt") && json["targetAt"].isInt64())
    dto.targetAt = json["targetAt"].asInt64();

  START_VALIDATION(UpdateProjectDto, dto)
  IS_NOT_EMPTY_OPTIONAL(name)
  MAX_LENGTH_OPTIONAL(name, 160)
  MAX_LENGTH_OPTIONAL(description, 2000)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(startsAt)
  IS_POSITIVE_TIMESTAMP_OPTIONAL(targetAt)
  END_VALIDATION()
  return dto;
}
