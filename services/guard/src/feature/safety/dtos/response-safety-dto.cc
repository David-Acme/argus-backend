#include "response-safety-dto.hxx"

Json::Value ResponseSafetyStatusDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["duressEnabled"] = duressEnabled;
  json["hasPin"] = hasPin;
  return json;
}

Json::Value ResponsePanicDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["alertId"] = static_cast<Json::Int64>(alertId);
  json["sent"] = sent;
  json["repeated"] = repeated;
  return json;
}
