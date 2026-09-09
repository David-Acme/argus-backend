#include "resolve-invitation-dto.hxx"

ResolveInvitationDto ResolveInvitationDto::fromJson(const Json::Value& json)
{
  ResolveInvitationDto dto;
  dto.token = json.get("token", "").asString();

  START_VALIDATION(ResolveInvitationDto, dto)
  IS_NOT_EMPTY(token)
  MIN_LENGTH(token, 32)
  MAX_LENGTH(token, 128)
  HAS_NO_SPACES(token)
  END_VALIDATION()
  return dto;
}
