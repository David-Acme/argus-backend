#include "module-catalog-file.hxx"

#include <config/settings-config.hxx>
#include <feature/modules/services/module-resolver.hxx>
#include <json/reader.h>
#include <settings/component-host.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

namespace
{
constexpr std::size_t kMaxIdLength = 32;
constexpr std::size_t kMaxTextLength = 240;
constexpr std::size_t kMaxPathLength = 256;
constexpr std::size_t kMaxUrlLength = 512;
constexpr std::size_t kMaxCommandLength = 200;
constexpr std::size_t kRevisionLength = 40;
constexpr std::int64_t kMaxRamMb = 1048576;
constexpr std::int64_t kMaxFileBytes = 1099511627776;

class CatalogError : public std::invalid_argument
{
public:
  using std::invalid_argument::invalid_argument;
};

struct Node
{
  const Json::Value& value;
  std::string where;
};

[[noreturn]] void reject(const std::string& problem)
{
  throw CatalogError(problem);
}

Node child(const Node& node, const std::string& name)
{
  return {.value = node.value[name], .where = node.where + "." + name};
}

Node element(const Node& list, Json::ArrayIndex index)
{
  return {.value = list.value[index], .where = list.where + "[" + std::to_string(index) + "]"};
}

void requireObject(const Node& node)
{
  if (!node.value.isObject())
    reject(node.where + ": must be an object");
}

const Json::Value& arrayAt(const Node& node)
{
  if (!node.value.isNull() && !node.value.isArray())
    reject(node.where + ": must be an array");
  return node.value;
}

std::string text(const Node& node, std::size_t maxLength)
{
  if (!node.value.isString() || node.value.asString().empty() || node.value.asString().size() > maxLength)
    reject(node.where + ": must be a non-empty string of at most " + std::to_string(maxLength) + " characters");
  return node.value.asString();
}

std::string optionalText(const Node& node, std::size_t maxLength)
{
  if (node.value.isNull())
    return {};
  return text(node, maxLength);
}

bool wellFormedId(const std::string& id)
{
  return !id.empty() && id.size() <= kMaxIdLength && std::ranges::all_of(id, [](char letter) {
    return (letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') || letter == '-';
  });
}

std::string identifier(const Node& node)
{
  auto id = text(node, kMaxIdLength);
  if (!wellFormedId(id))
    reject(node.where + ": must hold only lowercase letters, digits and '-'");
  return id;
}

std::int64_t integer(const Node& node, std::int64_t maxValue)
{
  if (!node.value.isIntegral() || node.value.asInt64() < 0 || node.value.asInt64() > maxValue)
    reject(node.where + ": must be an integer between 0 and " + std::to_string(maxValue));
  return node.value.asInt64();
}

std::vector<std::string> identifiers(const Node& node)
{
  std::vector<std::string> ids;
  const auto& list = arrayAt(node);
  for (Json::ArrayIndex index = 0; index < list.size(); ++index) {
    auto id = identifier(element(node, index));
    if (std::ranges::find(ids, id) != ids.end())
      reject(node.where + ": repeats " + id);
    ids.push_back(std::move(id));
  }
  return ids;
}

std::vector<std::string> words(const Node& node)
{
  std::vector<std::string> list;
  const auto& values = arrayAt(node);
  list.reserve(values.size());
  for (Json::ArrayIndex index = 0; index < values.size(); ++index)
    list.push_back(identifier(element(node, index)));
  return list;
}

LocalizedText localized(const Node& node)
{
  requireObject(node);
  return {.es = text(child(node, "es"), kMaxTextLength), .en = text(child(node, "en"), kMaxTextLength)};
}

bool pinnedUrl(const std::string& url)
{
  if (url.find("/releases/download/") != std::string::npos)
    return true;
  std::size_t start = 0;
  while (start < url.size()) {
    const auto end = std::min(url.find('/', start), url.size());
    const std::string_view segment(url.data() + start, end - start);
    if (segment.size() == kRevisionLength &&
        std::ranges::all_of(segment, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
      return true;
    start = end + 1;
  }
  return false;
}

ComponentFile readFile(const Node& node, ComponentSource source)
{
  requireObject(node);
  ComponentFile file{.path = text(child(node, "path"), kMaxPathLength),
                     .url = optionalText(child(node, "url"), kMaxUrlLength),
                     .sizeBytes = integer(child(node, "sizeBytes"), kMaxFileBytes),
                     .sha256 = optionalText(child(node, "sha256"), kMaxTextLength)};
  if (file.sizeBytes == 0)
    reject(node.where + ".sizeBytes: must be positive");
  if (source == ComponentSource::Download && !pinnedUrl(file.url))
    reject(node.where + ".url: a download must name a pinned revision or a release asset");
  return file;
}

bool knownOwner(const std::string& owner)
{
  return std::ranges::find(kSettingsOwnerOrder, owner) != kSettingsOwnerOrder.end();
}

bool knownDataOwner(const std::string& owner)
{
  return knownOwner(owner) || std::ranges::find(kSettingsDataOwnerOrder, owner) != kSettingsDataOwnerOrder.end();
}

CatalogComponent readComponent(const Node& node)
{
  requireObject(node);
  const auto sourceNode = child(node, "source");
  const auto source = componentSourceFromString(text(sourceNode, kMaxIdLength));
  if (!source)
    reject(sourceNode.where + ": must be download or provisioned");
  CatalogComponent component{.spec = {.id = identifier(child(node, "id")),
                                      .source = *source,
                                      .files = {},
                                      .hostCommand = optionalText(child(node, "hostCommand"), kMaxCommandLength)},
                             .owner = text(child(node, "owner"), kMaxIdLength),
                             .ramMb = integer(child(node, "ramMb"), kMaxRamMb)};
  if (!knownOwner(component.owner))
    reject(node.where + ".owner: is not a settings owner");
  const auto files = child(node, "files");
  const auto& list = arrayAt(files);
  component.spec.files.reserve(list.size());
  for (Json::ArrayIndex index = 0; index < list.size(); ++index)
    component.spec.files.push_back(readFile(element(files, index), *source));
  if (component.spec.source == ComponentSource::Provisioned && component.spec.hostCommand.empty())
    reject(node.where + ".hostCommand: a provisioned component names the host command that installs it");
  if (!DiskComponentHost::acceptable(component.spec))
    reject(node.where + ": every file needs a relative path and a size, and a download a https URL and a SHA-256");
  return component;
}

HardwareRequirement readHardware(const Node& node)
{
  requireObject(node);
  const auto gpu = child(node, "recommendedGpu");
  if (!gpu.value.isNull() && !gpu.value.isBool())
    reject(gpu.where + ": must be true or false");
  HardwareRequirement hardware{.minRamMb = integer(child(node, "minRamMb"), kMaxRamMb),
                               .recommendedRamMb = integer(child(node, "recommendedRamMb"), kMaxRamMb),
                               .requiredCpu = words(child(node, "requiredCpu")),
                               .recommendedCpu = words(child(node, "recommendedCpu")),
                               .recommendedGpu = gpu.value.isBool() && gpu.value.asBool()};
  if (hardware.recommendedRamMb < hardware.minRamMb)
    reject(node.where + ": recommendedRamMb is below minRamMb");
  return hardware;
}

std::string route(const Node& node)
{
  auto value = text(node, kMaxPathLength);
  if (value.front() != '/')
    reject(node.where + ": must start with /");
  return value;
}

CatalogModule readModule(const Node& node)
{
  requireObject(node);
  const auto kindNode = child(node, "kind");
  const auto kind = moduleKindFromString(text(kindNode, kMaxIdLength));
  if (!kind)
    reject(kindNode.where + ": must be core, available or coming_soon");
  CatalogModule module{.id = identifier(child(node, "id")),
                       .kind = *kind,
                       .name = localized(child(node, "name")),
                       .summary = localized(child(node, "summary")),
                       .required = identifiers(child(node, "requires")),
                       .components = identifiers(child(node, "components")),
                       .gates = {},
                       .dataOwners = identifiers(child(node, "dataOwners")),
                       .hardware = readHardware(child(node, "hardware")),
                       .gettingStarted = {}};
  const auto gates = child(node, "gates");
  for (Json::ArrayIndex index = 0; index < arrayAt(gates).size(); ++index)
    module.gates.push_back(route(element(gates, index)));
  const auto steps = child(node, "gettingStarted");
  for (Json::ArrayIndex index = 0; index < arrayAt(steps).size(); ++index) {
    const auto step = element(steps, index);
    requireObject(step);
    GettingStartedItem item{
        .id = identifier(child(step, "id")), .title = localized(child(step, "title")), .route = route(child(step, "route"))};
    if (std::ranges::find(module.gettingStarted, item.id, &GettingStartedItem::id) != module.gettingStarted.end())
      reject(step.where + ".id: repeats " + item.id);
    module.gettingStarted.push_back(std::move(item));
  }
  for (const auto& owner : module.dataOwners)
    if (!knownDataOwner(owner))
      reject(node.where + ".dataOwners: " + owner + " is not a service that keeps module data");
  if (module.kind == ModuleKind::ComingSoon && !module.components.empty())
    reject(node.where + ": a coming_soon module installs nothing");
  if (module.kind == ModuleKind::Core && !module.required.empty())
    reject(node.where + ": the core module requires nothing");
  return module;
}

void crossCheck(const ModuleCatalog& catalog)
{
  std::set<std::string> ids;
  for (const auto& component : catalog.components)
    if (!ids.insert(component.spec.id).second)
      reject("module catalog.components: repeats the id " + component.spec.id);
  ids.clear();
  for (const auto& module : catalog.modules)
    if (!ids.insert(module.id).second)
      reject("module catalog.modules: repeats the id " + module.id);
  if (std::ranges::count(catalog.modules, ModuleKind::Core, &CatalogModule::kind) != 1)
    reject("module catalog.modules: exactly one module is core");
  std::set<std::string> gates;
  for (const auto& module : catalog.modules) {
    for (const auto& required : module.required)
      if (catalog.module(required) == nullptr || required == module.id)
        reject("module catalog." + module.id + ".requires: names an unknown module " + required);
    for (const auto& component : module.components)
      if (catalog.component(component) == nullptr)
        reject("module catalog." + module.id + ".components: names an unknown component " + component);
    for (const auto& gate : module.gates)
      if (!gates.insert(gate).second)
        reject("module catalog." + module.id + ".gates: " + gate + " is gated by two modules");
  }
  if (const auto cycle = module_resolver::cycleThrough(catalog))
    reject("module catalog." + *cycle + ".requires: the dependencies form a cycle");
}

ModuleCatalog readCatalog(const Json::Value& root)
{
  const Node file{.value = root, .where = "module catalog"};
  requireObject(file);
  ModuleCatalog catalog;
  const auto components = child(file, "components");
  for (Json::ArrayIndex index = 0; index < arrayAt(components).size(); ++index)
    catalog.components.push_back(readComponent(element(components, index)));
  const auto modules = child(file, "modules");
  if (!modules.value.isArray() || modules.value.empty())
    reject(modules.where + ": must be a non-empty array");
  for (Json::ArrayIndex index = 0; index < modules.value.size(); ++index)
    catalog.modules.push_back(readModule(element(modules, index)));
  crossCheck(catalog);
  return catalog;
}
}

ModuleCatalogParse parseModuleCatalog(const Json::Value& root)
{
  try {
    return {.catalog = readCatalog(root), .problem = {}};
  }
  catch (const CatalogError& error) {
    return {.catalog = std::nullopt, .problem = error.what()};
  }
}

std::optional<ModuleCatalog> loadModuleCatalog(const std::string& path)
{
  std::ifstream file(path);
  if (!file) {
    LOG_ERROR << "Module catalog: " << path << " could not be opened; the module routes answer 503";
    return std::nullopt;
  }
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  if (!Json::parseFromStream(builder, file, &root, &errors)) {
    LOG_ERROR << "Module catalog: " << path << " is not valid JSON (" << errors << "); the module routes answer 503";
    return std::nullopt;
  }
  auto parsed = parseModuleCatalog(root);
  if (!parsed.catalog) {
    LOG_ERROR << "Module catalog: " << path << " " << parsed.problem << "; the module routes answer 503";
    return std::nullopt;
  }
  LOG_INFO << "Module catalog: " << parsed.catalog->modules.size() << " modules and "
           << parsed.catalog->components.size() << " components loaded from " << path;
  return std::move(parsed.catalog);
}
