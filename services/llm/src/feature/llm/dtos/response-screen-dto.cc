#include "response-screen-dto.hxx"

Json::Value ResponseScreenDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["text"] = text;
  return json;
}
