#include "response-login-dto.hxx"

Json::Value ResponseLoginDto::toJson() const
{
  Json::Value json;
  json["accessToken"] = accessToken;
  json["refreshToken"] = refreshToken;
  json["userId"] = userId;
  json["name"] = name;
  json["role"] = userRoleToString(role);
  json["personId"] = personId;
  return json;
}
