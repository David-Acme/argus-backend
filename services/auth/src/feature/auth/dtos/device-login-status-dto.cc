#include "device-login-status-dto.hxx"

Json::Value DeviceLoginStatusDto::toJson() const
{
  Json::Value json;
  json["status"] = deviceLoginStatusToString(status);
  if (status == DeviceLoginStatus::Approved) {
    json["accessToken"] = accessToken;
    json["refreshToken"] = refreshToken;
    json["userId"] = static_cast<Json::Int64>(userId);
    json["name"] = name;
    json["role"] = userRoleToString(role);
    if (!deviceSecret.empty())
      json["device_secret"] = deviceSecret;
  }
  return json;
}
