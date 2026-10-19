#include "response-rtc-ice-dto.hxx"

Json::Value ResponseRtcIceDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["iceServers"] = iceServers;
  json["expiresAt"] = static_cast<Json::Int64>(expiresAt);
  return json;
}
