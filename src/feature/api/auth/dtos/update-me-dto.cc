#include "update-me-dto.hxx"

UpdateMeDto UpdateMeDto::fromJson(const Json::Value& json)
{
  UpdateMeDto dto;
  if (json.isMember("name") && json["name"].isString())
    dto.name = json["name"].asString();

  START_VALIDATION(UpdateMeDto, dto)
  MAX_LENGTH_OPTIONAL(name, 120)
  END_VALIDATION()

  return dto;
}
