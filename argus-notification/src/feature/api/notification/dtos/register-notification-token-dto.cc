#include "register-notification-token-dto.hxx"

RegisterNotificationTokenDto
RegisterNotificationTokenDto::fromJson(const Json::Value& json)
{
  RegisterNotificationTokenDto dto;
  dto.token = json.get("token", "").asString();
  dto.platform = json.get("platform", "").asString();
  dto.lang = json.get("lang", "").asString();

  START_VALIDATION(RegisterNotificationTokenDto, dto)
  IS_NOT_EMPTY(token)
  MAX_LENGTH(token, 512)
  MAX_LENGTH(platform, 32)
  MAX_LENGTH(lang, 8)
  END_VALIDATION()

  return dto;
}
