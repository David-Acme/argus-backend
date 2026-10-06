#pragma once

#include <feature/settings/services/settings-gateway-service.hxx>

#include <functional>
#include <string>
#include <vector>

namespace owner_visibility
{
[[nodiscard]] std::vector<OwnerCatalog> visible(std::vector<OwnerCatalog> catalogs,
                                                const std::function<bool(const std::string&)>& ownerVisible);
}
