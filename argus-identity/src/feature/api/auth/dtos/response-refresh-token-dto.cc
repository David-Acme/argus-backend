#include "response-refresh-token-dto.hxx"

Json::Value ResponseRefreshTokenDto::toJson() const
{
  Json::Value json;
  json["accessToken"] = accessToken;
  json["refreshToken"] = refreshToken;
  return json;
}
