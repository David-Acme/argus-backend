#pragma once

#include <feature/modules/schemas/module-catalog.hxx>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace module_resolver
{
[[nodiscard]] std::optional<std::string> cycleThrough(const ModuleCatalog& catalog);

[[nodiscard]] std::vector<std::string> installOrder(const ModuleCatalog& catalog, const std::string& id);

[[nodiscard]] std::vector<std::string> enabledDependents(const ModuleCatalog& catalog,
                                                         const std::string& id,
                                                         const std::set<std::string>& enabled);
}
