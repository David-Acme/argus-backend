#include "create-invitation-dto.hxx"

CreateInvitationDto CreateInvitationDto::fromJson(const Json::Value& json)
{
  CreateInvitationDto dto;
  dto.role = json.get("role", userRoleToString(UserRole::Guest)).asString();

  START_VALIDATION(CreateInvitationDto, dto)
  CUSTOM_LAMBDA(role, [](const CreateInvitationDto& value)
                    -> std::optional<std::string> {
    const UserRole role = userRoleFromString(value.role);
    if (userRoleToString(role) != value.role || role == UserRole::Owner)
      return "role must be resident, guard, or guest";
    return std::nullopt;
  })
  END_VALIDATION()
  dto.userRole = userRoleFromString(dto.role);
  return dto;
}
