#include "response-device-login-details-dto.hxx"

Json::Value ResponseDeviceLoginDetailsDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["challengeId"] = challengeId;
  json["platform"] = sessionPlatformToString(platform);
  json["deviceName"] = deviceName;
  json["origin"] = sessionOriginToString(origin);
  json["ipAddress"] = ipAddress;
  json["createdAt"] = static_cast<Json::Int64>(createdAt);
  json["expiresAt"] = static_cast<Json::Int64>(expiresAt);
  return json;
}
