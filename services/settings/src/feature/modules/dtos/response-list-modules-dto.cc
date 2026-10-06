#include "response-list-modules-dto.hxx"

#include <feature/modules/dtos/module-json.hxx>

#include <utility>

Json::Value ResponseListModulesDto::toJson() const
{
  Json::Value list(Json::arrayValue);
  for (const auto& view : modules)
    list.append(owner ? module_json::module(view, lang) : module_json::member(view, lang));
  Json::Value json(Json::objectValue);
  json["modules"] = std::move(list);
  return json;
}
