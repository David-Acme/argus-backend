#include "response-camera-patrol-dto.hxx"

Json::Value ResponseCameraPatrolDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["patrol"] = patrol ? patrol->toJson() : Json::Value();
  return json;
}
