#include "create-device-login-dto.hxx"

Json::Value CreateDeviceLoginDto::toJson() const
{
  Json::Value json;
  json["challengeId"] = challengeId;
  json["expiresAt"] = Json::Int64(expiresAt);
  return json;
}