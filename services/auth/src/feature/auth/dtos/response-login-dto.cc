#include "response-login-dto.hxx"

Json::Value ResponseLoginDto::toJson() const
{
  Json::Value json;
  json["accessToken"] = accessToken;
  json["refreshToken"] = refreshToken;
  json["userId"] = static_cast<Json::Int64>(userId);
  json["name"] = name;
  json["role"] = userRoleToString(role);
  json["personId"] = static_cast<Json::Int64>(personId);
  json["alreadyRegistered"] = alreadyRegistered;
  if (!deviceSecret.empty())
    json["device_secret"] = deviceSecret;
  return json;
}
