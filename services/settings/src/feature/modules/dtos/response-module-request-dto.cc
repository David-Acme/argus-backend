#include "response-module-request-dto.hxx"

Json::Value ResponseModuleRequestDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["moduleId"] = request.moduleId;
  json["requested"] = true;
  json["duplicate"] = request.duplicate;
  return json;
}
