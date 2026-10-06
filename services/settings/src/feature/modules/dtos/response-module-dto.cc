#include "response-module-dto.hxx"

#include <feature/modules/dtos/module-json.hxx>

Json::Value ResponseModuleDto::toJson() const
{
  return module_json::module(module, lang);
}

Json::Value ResponseModuleJobDto::toJson() const
{
  return module_json::job(job);
}
