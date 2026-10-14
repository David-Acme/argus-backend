#include "unregister-notification-token-dto.hxx"

UnregisterNotificationTokenDto
UnregisterNotificationTokenDto::fromJson(const Json::Value& json)
{
  UnregisterNotificationTokenDto dto;
  dto.token = json.get("token", "").asString();

  START_VALIDATION(UnregisterNotificationTokenDto, dto)
  IS_NOT_EMPTY(token)
  MAX_LENGTH(token, 512)
  END_VALIDATION()

  return dto;
}
