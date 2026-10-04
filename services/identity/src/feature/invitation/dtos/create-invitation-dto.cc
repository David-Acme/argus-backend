#include "create-invitation-dto.hxx"

CreateInvitationDto CreateInvitationDto::fromJson(const Json::Value& json)
{
  CreateInvitationDto dto;
  dto.roleValue = json.get("role", "guest").asString();

  START_VALIDATION(CreateInvitationDto, dto)
  CUSTOM_LAMBDA(role, [](const CreateInvitationDto& value)
                    -> std::optional<std::string> {
    if (value.roleValue != "resident" && value.roleValue != "guard" &&
        value.roleValue != "guest")
      return "role must be resident, guard, or guest";
    return std::nullopt;
  })
  END_VALIDATION()
  dto.role = userRoleFromString(dto.roleValue);
  return dto;
}
