#include "device-login-status-dto.hxx"

Json::Value DeviceLoginStatusDto::toJson() const
{
  Json::Value json;
  json["status"] = status;
  if (status == "approved") {
    json["accessToken"] = accessToken;
    json["refreshToken"] = refreshToken;
    json["userId"] = userId;
    json["name"] = name;
    json["role"] = userRoleToString(role);
  }
  return json;
}