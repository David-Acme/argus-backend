#include "module-gate.hxx"

#include <auth/role-access.hxx>
#include <text/json-util.hxx>

#include <json/value.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace
{
constexpr std::size_t kMaxModuleIdBytes = 64;
constexpr std::size_t kMaxModules = 64;
constexpr std::size_t kMaxRoles = 8;
constexpr std::size_t kMaxRoleBytes = 32;
constexpr std::size_t kMaxNameBytes = 120;
constexpr std::size_t kMaxSummaryBytes = 400;
constexpr std::size_t kMaxWhatBytes = 600;
constexpr std::size_t kMaxExamples = 5;
constexpr std::size_t kMaxExampleBytes = 200;
constexpr std::size_t kMaxKindBytes = 32;

bool validIdentifier(std::string_view id, std::size_t maxBytes)
{
  return !id.empty() && id.size() <= maxBytes &&
         std::ranges::all_of(id, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
         });
}

bool validModuleId(std::string_view id)
{
  return validIdentifier(id, kMaxModuleIdBytes);
}

bool parseText(const Json::Value& json, std::size_t maxBytes, std::string& out)
{
  if (json.isNull())
    return true;
  if (!json.isString() || json.asString().size() > maxBytes)
    return false;
  out = json.asString();
  return true;
}

bool parseLocalized(const Json::Value& json, std::size_t maxBytes, ModuleText& out)
{
  if (json.isNull())
    return true;
  if (!json.isObject())
    return false;
  return parseText(json[std::string(module_gate::kEsField)], maxBytes, out.es) &&
         parseText(json[std::string(module_gate::kEnField)], maxBytes, out.en);
}

bool parseIntroLine(const Json::Value& json, ModuleIntroText& out)
{
  if (json.isNull())
    return true;
  if (!json.isObject() ||
      !parseText(json[std::string(module_gate::kWhatField)], kMaxWhatBytes, out.what))
    return false;
  const Json::Value& examples = json[std::string(module_gate::kExamplesField)];
  if (examples.isNull())
    return true;
  if (!examples.isArray() || examples.size() > kMaxExamples)
    return false;
  for (const auto& example : examples) {
    if (!example.isString() || example.asString().size() > kMaxExampleBytes)
      return false;
    out.examples.push_back(example.asString());
  }
  return true;
}

bool parseIntro(const Json::Value& json, ModuleIntro& out)
{
  if (json.isNull())
    return true;
  if (!json.isObject())
    return false;
  return parseIntroLine(json[std::string(module_gate::kEsField)], out.es) &&
         parseIntroLine(json[std::string(module_gate::kEnField)], out.en);
}

bool parseRoles(const Json::Value& json, std::vector<std::string>& out)
{
  if (json.isNull())
    return true;
  if (!json.isArray() || json.size() > kMaxRoles)
    return false;
  for (const auto& role : json) {
    if (!role.isString() || !validIdentifier(role.asString(), kMaxRoleBytes))
      return false;
    out.push_back(role.asString());
  }
  return true;
}

Json::Value localizedJson(const ModuleText& text)
{
  Json::Value json(Json::objectValue);
  json[std::string(module_gate::kEsField)] = text.es;
  json[std::string(module_gate::kEnField)] = text.en;
  return json;
}

Json::Value introLineJson(const ModuleIntroText& line)
{
  Json::Value json(Json::objectValue);
  json[std::string(module_gate::kWhatField)] = line.what;
  Json::Value examples(Json::arrayValue);
  for (const auto& example : line.examples)
    examples.append(example);
  json[std::string(module_gate::kExamplesField)] = std::move(examples);
  return json;
}
}

bool ModuleGate::enabled(std::string_view module) const
{
  if (module == kCoreModule)
    return true;
  std::scoped_lock lock(mutex_);
  const auto found = states_.find(std::string(module));
  return found == states_.end() || found->second.enabled;
}

std::optional<std::string> ModuleGate::disabledModuleOf(std::string_view path) const
{
  const auto module = role_access::moduleOfPath(path);
  if (!module || enabled(*module))
    return std::nullopt;
  return std::string(*module);
}

std::optional<std::string> ModuleGate::disabledModuleOf(std::string_view path,
                                                        drogon::HttpMethod method) const
{
  const auto module = role_access::moduleOfRoute(path, method);
  if (!module || enabled(*module))
    return std::nullopt;
  return std::string(*module);
}

namespace
{
ModuleFlags sortedFlags(const std::unordered_map<std::string, ModuleFlag>& states)
{
  ModuleFlags flags;
  flags.reserve(states.size());
  for (const auto& [id, flag] : states)
    flags.push_back(flag);
  std::ranges::sort(flags, {}, &ModuleFlag::id);
  return flags;
}
}

ModuleFlags ModuleGate::known() const
{
  std::scoped_lock lock(mutex_);
  return sortedFlags(states_);
}

ModuleSnapshot ModuleGate::snapshot() const
{
  return *current();
}

std::shared_ptr<const ModuleSnapshot> ModuleGate::current() const
{
  std::scoped_lock lock(mutex_);
  return current_;
}

bool ModuleGate::roleActive(UserRole role) const
{
  return current()->roleActive(role);
}

std::vector<ModuleChange> ModuleGate::apply(const ModuleFlags& flags)
{
  std::vector<ModuleChange> changes;
  std::vector<Listener> listeners;
  std::vector<StateListener> stateListeners;
  bool stateChanged = false;
  {
    std::scoped_lock lock(mutex_);
    for (const auto& flag : flags) {
      const auto found = states_.find(flag.id);
      const bool previous = found == states_.end() || found->second.enabled;
      if (found == states_.end() || !(found->second == flag))
        stateChanged = true;
      states_[flag.id] = flag;
      if (previous != flag.enabled)
        changes.push_back({.id = flag.id, .enabled = flag.enabled});
    }
    if (stateChanged)
      current_ = std::make_shared<const ModuleSnapshot>(sortedFlags(states_));
    if (!changes.empty())
      listeners = listeners_;
    if (stateChanged)
      stateListeners = stateListeners_;
  }
  for (const auto& change : changes) {
    LOG_INFO << "Modules: " << change.id << (change.enabled ? " enabled" : " disabled");
    for (const auto& listener : listeners)
      listener(change);
  }
  for (const auto& listener : stateListeners)
    listener();
  return changes;
}

void ModuleGate::onChange(Listener listener)
{
  std::scoped_lock lock(mutex_);
  listeners_.push_back(std::move(listener));
}

void ModuleGate::onStateChange(StateListener listener)
{
  std::scoped_lock lock(mutex_);
  stateListeners_.push_back(std::move(listener));
}

void ModuleGate::reset()
{
  std::scoped_lock lock(mutex_);
  states_.clear();
  current_ = std::make_shared<const ModuleSnapshot>();
  listeners_.clear();
  stateListeners_.clear();
}

ModuleGate& moduleGate()
{
  static ModuleGate gate;
  return gate;
}

namespace module_gate
{
std::optional<ModuleFlags> parseEnabledSet(std::string_view body)
{
  const Json::Value json = json_util::fromString(std::string(body));
  if (!json.isObject())
    return std::nullopt;
  const Json::Value& modules = json[std::string(kModulesField)];
  if (!modules.isArray() || modules.size() > kMaxModules)
    return std::nullopt;
  ModuleFlags flags;
  flags.reserve(modules.size());
  for (const auto& entry : modules) {
    if (!entry.isObject())
      return std::nullopt;
    const Json::Value& id = entry[std::string(kIdField)];
    const Json::Value& enabled = entry[std::string(kEnabledField)];
    const Json::Value& lifecycle = entry[std::string(kLifecycleField)];
    const Json::Value& purgedAt = entry[std::string(kDataPurgedAtField)];
    if (!id.isString() || !enabled.isBool() || !validModuleId(id.asString()) ||
        !(lifecycle.isNull() || lifecycle.isString()) ||
        !(purgedAt.isNull() || purgedAt.isIntegral()))
      return std::nullopt;
    ModuleFlag flag;
    flag.id = id.asString();
    if (!lifecycle.isNull())
      flag.lifecycle = lifecycle.asString();
    const bool active = lifecycle.isNull() || flag.lifecycle == kActiveLifecycle;
    flag.enabled = enabled.asBool() && active;
    flag.dataPurgedAt = purgedAt.isNull() ? 0 : purgedAt.asInt64();
    if (!parseRoles(entry[std::string(kRolesField)], flag.roles) ||
        !parseLocalized(entry[std::string(kNameField)], kMaxNameBytes, flag.name) ||
        !parseLocalized(entry[std::string(kSummaryField)], kMaxSummaryBytes, flag.summary) ||
        !parseIntro(entry[std::string(kIntroField)], flag.intro) ||
        !parseText(entry[std::string(kKindField)], kMaxKindBytes, flag.kind))
      return std::nullopt;
    flags.push_back(std::move(flag));
  }
  return flags;
}

std::string serializeEnabledSet(const ModuleFlags& flags)
{
  Json::Value modules(Json::arrayValue);
  for (const auto& flag : flags) {
    Json::Value entry(Json::objectValue);
    entry[std::string(kIdField)] = flag.id;
    entry[std::string(kEnabledField)] = flag.enabled;
    if (!flag.lifecycle.empty())
      entry[std::string(kLifecycleField)] = flag.lifecycle;
    if (flag.dataPurgedAt > 0)
      entry[std::string(kDataPurgedAtField)] = static_cast<Json::Int64>(flag.dataPurgedAt);
    Json::Value roles(Json::arrayValue);
    for (const auto& role : flag.roles)
      roles.append(role);
    entry[std::string(kRolesField)] = std::move(roles);
    entry[std::string(kNameField)] = localizedJson(flag.name);
    entry[std::string(kSummaryField)] = localizedJson(flag.summary);
    Json::Value intro(Json::objectValue);
    intro[std::string(kEsField)] = introLineJson(flag.intro.es);
    intro[std::string(kEnField)] = introLineJson(flag.intro.en);
    entry[std::string(kIntroField)] = std::move(intro);
    if (!flag.kind.empty())
      entry[std::string(kKindField)] = flag.kind;
    modules.append(std::move(entry));
  }
  Json::Value json(Json::objectValue);
  json[std::string(kModulesField)] = std::move(modules);
  return json_util::toString(json);
}

std::optional<ModuleFlags> loadStateFile(const std::string& path)
{
  if (path.empty())
    return std::nullopt;
  std::ifstream file(path);
  if (!file)
    return std::nullopt;
  std::ostringstream content;
  content << file.rdbuf();
  auto flags = parseEnabledSet(content.str());
  if (!flags)
    LOG_WARN << "Modules: the last known state at " << path
             << " is unreadable; every module counts as enabled until the settings feed answers";
  return flags;
}

bool saveStateFile(const std::string& path, const ModuleFlags& flags)
{
  if (path.empty())
    return false;
  const std::filesystem::path target(path);
  std::error_code error;
  if (target.has_parent_path())
    std::filesystem::create_directories(target.parent_path(), error);
  const std::filesystem::path partial(path + ".part");
  {
    std::ofstream file(partial, std::ios::trunc);
    if (!file)
      return false;
    file << serializeEnabledSet(flags);
    if (!file.flush())
      return false;
  }
  std::filesystem::rename(partial, target, error);
  return !error;
}
}
