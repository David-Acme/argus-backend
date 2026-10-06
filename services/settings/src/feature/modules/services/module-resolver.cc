#include "module-resolver.hxx"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace
{
enum class Mark : std::uint8_t
{
  Visiting,
  Done
};

struct Walk
{
  const ModuleCatalog& catalog;
  std::map<std::string, Mark> marks;
  std::vector<std::string> order;
};

bool visit(Walk& walk, const std::string& id)
{
  const auto mark = walk.marks.find(id);
  if (mark != walk.marks.end())
    return mark->second == Mark::Done;
  walk.marks[id] = Mark::Visiting;
  if (const auto* module = walk.catalog.module(id)) {
    for (const auto& required : module->required)
      if (!visit(walk, required))
        return false;
  }
  walk.marks[id] = Mark::Done;
  walk.order.push_back(id);
  return true;
}
}

namespace module_resolver
{
std::optional<std::string> cycleThrough(const ModuleCatalog& catalog)
{
  for (const auto& module : catalog.modules) {
    Walk walk{.catalog = catalog, .marks = {}, .order = {}};
    if (!visit(walk, module.id))
      return module.id;
  }
  return std::nullopt;
}

std::vector<std::string> installOrder(const ModuleCatalog& catalog, const std::string& id)
{
  Walk walk{.catalog = catalog, .marks = {}, .order = {}};
  if (!visit(walk, id))
    throw std::logic_error("The module catalog has a dependency cycle through " + id);
  return walk.order;
}

std::vector<std::string> enabledDependents(const ModuleCatalog& catalog,
                                           const std::string& id,
                                           const std::set<std::string>& enabled)
{
  std::vector<std::string> dependents;
  for (const auto& module : catalog.modules) {
    if (module.id == id || !enabled.contains(module.id))
      continue;
    const auto order = installOrder(catalog, module.id);
    if (std::ranges::find(order, id) != order.end())
      dependents.push_back(module.id);
  }
  return dependents;
}
}
