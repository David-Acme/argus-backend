#include "update-user-dto.hxx"

UpdateUserDto UpdateUserDto::fromJson(const Json::Value& json)
{
  UpdateUserDto dto;
  if (json.isMember("name") && json["name"].isString())
    dto.name = json["name"].asString();
  if (json.isMember("lastName") && json["lastName"].isString())
    dto.lastName = json["lastName"].asString();
  if (json.isMember("role") && json["role"].isString())
    dto.roleValue = json["role"].asString();
  if (json.isMember("isActive") && json["isActive"].isBool())
    dto.isActive = json["isActive"].asBool();

  START_VALIDATION(UpdateUserDto, dto)
  IS_NOT_EMPTY_OPTIONAL(name)
  MAX_LENGTH_OPTIONAL(name, 120)
  MAX_LENGTH_OPTIONAL(lastName, 120)
  CUSTOM_LAMBDA(role, [](const UpdateUserDto& value)
                    -> std::optional<std::string> {
    if (value.roleValue && *value.roleValue != "owner" &&
        *value.roleValue != "resident" && *value.roleValue != "guard" &&
        *value.roleValue != "guest")
      return "role must be owner, resident, guard, or guest";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(body, [](const UpdateUserDto& value)
                         -> std::optional<std::string> {
    if (!value.name && !value.lastName && !value.roleValue && !value.isActive)
      return "at least one editable field is required";
    return std::nullopt;
  })
  END_VALIDATION()
  if (dto.roleValue)
    dto.role = userRoleFromString(*dto.roleValue);
  return dto;
}
