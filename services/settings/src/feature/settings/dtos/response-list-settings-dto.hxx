#pragma once

#include <feature/settings/services/settings-gateway-service.hxx>
#include <json/value.h>

#include <vector>

struct ResponseListSettingsDto
{
  const std::vector<OwnerCatalog>& owners;

  [[nodiscard]] Json::Value toJson() const;
};
