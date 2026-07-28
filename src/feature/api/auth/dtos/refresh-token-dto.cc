#include "refresh-token-dto.hxx"

RefreshTokenDto RefreshTokenDto::form_json(const Json::Value& json)
{
  RefreshTokenDto dto;
  dto.refreshToken = json.get("refreshToken", "").asString();

  START_VALIDATION(RefreshTokenDto, dto)
  IS_NOT_EMPTY(refreshToken)
  END_VALIDATION()

  return dto;
}
