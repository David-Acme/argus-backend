#include "create-invitation-dto.hxx"

CreateInvitationDto CreateInvitationDto::fromJson(const Json::Value& json)
{
  CreateInvitationDto dto;
  dto.role = json.get("role", "").asString();

  START_VALIDATION(CreateInvitationDto, dto)
  CUSTOM_LAMBDA(role, [](const CreateInvitationDto& value)
                    -> std::optional<std::string> {
    const auto role = parseUserRole(value.role);
    if (!role || *role == UserRole::Owner)
      return "role must be resident, guard, or guest";
    return std::nullopt;
  })
  END_VALIDATION()
  dto.userRole = userRoleFromString(dto.role);
  return dto;
}
