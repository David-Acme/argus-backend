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

bool validModuleId(std::string_view id)
{
  return !id.empty() && id.size() <= kMaxModuleIdBytes &&
         std::ranges::all_of(id, [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
         });
}
}

bool ModuleGate::enabled(std::string_view module) const
{
  std::scoped_lock lock(mutex_);
  const auto found = states_.find(std::string(module));
  return found == states_.end() || found->second;
}

std::optional<std::string> ModuleGate::disabledModuleOf(std::string_view path) const
{
  const auto module = role_access::moduleOfPath(path);
  if (!module || enabled(*module))
    return std::nullopt;
  return std::string(*module);
}

ModuleFlags ModuleGate::known() const
{
  std::scoped_lock lock(mutex_);
  ModuleFlags flags;
  flags.reserve(states_.size());
  for (const auto& [id, enabled] : states_)
    flags.push_back({.id = id, .enabled = enabled});
  std::ranges::sort(flags, {}, &ModuleFlag::id);
  return flags;
}

std::vector<ModuleChange> ModuleGate::apply(const ModuleFlags& flags)
{
  std::vector<ModuleChange> changes;
  std::vector<Listener> listeners;
  {
    std::scoped_lock lock(mutex_);
    for (const auto& flag : flags) {
      const auto found = states_.find(flag.id);
      const bool previous = found == states_.end() || found->second;
      states_[flag.id] = flag.enabled;
      if (previous != flag.enabled)
        changes.push_back({.id = flag.id, .enabled = flag.enabled});
    }
    if (!changes.empty())
      listeners = listeners_;
  }
  for (const auto& change : changes) {
    LOG_INFO << "Modules: " << change.id << (change.enabled ? " enabled" : " disabled");
    for (const auto& listener : listeners)
      listener(change);
  }
  return changes;
}

void ModuleGate::onChange(Listener listener)
{
  std::scoped_lock lock(mutex_);
  listeners_.push_back(std::move(listener));
}

void ModuleGate::reset()
{
  std::scoped_lock lock(mutex_);
  states_.clear();
  listeners_.clear();
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
    if (!id.isString() || !enabled.isBool() || !validModuleId(id.asString()) ||
        !(lifecycle.isNull() || lifecycle.isString()))
      return std::nullopt;
    const bool active = lifecycle.isNull() || lifecycle.asString() == kActiveLifecycle;
    flags.push_back({.id = id.asString(), .enabled = enabled.asBool() && active});
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
