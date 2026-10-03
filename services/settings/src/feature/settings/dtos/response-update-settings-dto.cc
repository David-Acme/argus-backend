#include "response-update-settings-dto.hxx"

#include <feature/settings/dtos/response-owner-catalog-dto.hxx>

Json::Value ResponseUpdateSettingsDto::toJson() const
{
  Json::Value applied(Json::arrayValue);
  for (const auto& key : outcome.applied)
    applied.append(key);
  Json::Value info(Json::objectValue);
  info["applied"] = std::move(applied);
  info["catalog"] = ResponseOwnerCatalogDto{.catalog = outcome.catalog}.toJson();
  return info;
}
