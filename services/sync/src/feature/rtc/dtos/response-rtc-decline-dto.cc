#include "response-rtc-decline-dto.hxx"

Json::Value ResponseRtcDeclineDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["declined"] = declined;
  return json;
}
