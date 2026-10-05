#include "response-heartbeat-dto.hxx"

Json::Value ResponseHeartbeatDto::toJson() const
{
  return heartbeat.isObject() ? heartbeat : Json::Value(Json::objectValue);
}
