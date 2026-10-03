#include "response-list-settings-dto.hxx"

#include <feature/settings/dtos/response-owner-catalog-dto.hxx>

Json::Value ResponseListSettingsDto::toJson() const
{
  Json::Value list(Json::arrayValue);
  for (const auto& owner : owners)
    list.append(ResponseOwnerCatalogDto{.catalog = owner}.toJson());
  Json::Value info(Json::objectValue);
  info["owners"] = std::move(list);
  return info;
}
