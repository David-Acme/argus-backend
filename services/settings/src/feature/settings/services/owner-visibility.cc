#include "owner-visibility.hxx"

#include <algorithm>
#include <utility>

std::vector<OwnerCatalog> owner_visibility::visible(std::vector<OwnerCatalog> catalogs,
                                                    const std::function<bool(const std::string&)>& ownerVisible)
{
  if (!ownerVisible)
    return catalogs;
  std::erase_if(catalogs, [&ownerVisible](const OwnerCatalog& catalog) { return !ownerVisible(catalog.service); });
  return catalogs;
}
