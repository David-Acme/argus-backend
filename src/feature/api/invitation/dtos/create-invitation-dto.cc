#include "create-invitation-dto.hxx"

#include <ctime>

CreateInvitationDto CreateInvitationDto::fromJson(const Json::Value& json)
{
  CreateInvitationDto dto;
  dto.roleValue = json.get("role", "guest").asString();
  if (json.isMember("maxRedemptions") && json["maxRedemptions"].isInt())
    dto.maxRedemptions = json["maxRedemptions"].asInt();
  if (json.isMember("expiresAt") && json["expiresAt"].isInt64())
    dto.expiresAt = json["expiresAt"].asInt64();

  START_VALIDATION(CreateInvitationDto, dto)
  BETWEEN(maxRedemptions, 1, 100)
  CUSTOM_LAMBDA(role, [](const CreateInvitationDto& value)
                    -> std::optional<std::string> {
    if (value.roleValue != "resident" && value.roleValue != "guard" &&
        value.roleValue != "guest")
      return "role must be resident, guard, or guest";
    return std::nullopt;
  })
  CUSTOM_LAMBDA(expiresAt, [](const CreateInvitationDto& value)
                         -> std::optional<std::string> {
    if (value.expiresAt <= std::time(nullptr))
      return "expiresAt must be in the future";
    return std::nullopt;
  })
  END_VALIDATION()
  dto.role = userRoleFromString(dto.roleValue);
  return dto;
}
