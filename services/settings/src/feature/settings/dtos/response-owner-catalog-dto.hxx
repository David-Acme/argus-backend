#pragma once

#include <feature/settings/services/settings-gateway-service.hxx>
#include <json/value.h>

struct ResponseOwnerCatalogDto
{
  const OwnerCatalog& catalog;

  [[nodiscard]] Json::Value toJson() const;
};
