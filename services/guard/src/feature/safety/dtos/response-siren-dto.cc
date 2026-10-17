#include "response-siren-dto.hxx"

Json::Value ResponseSirenDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["cameraId"] = static_cast<Json::Int64>(cameraId);
  json["enabled"] = enabled;
  json["until"] = static_cast<Json::Int64>(until);
  return json;
}
