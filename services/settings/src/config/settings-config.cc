#include "settings-config.hxx"

#include <config/config-service.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace
{
constexpr int kMaxTimeoutMs = 120000;
constexpr const char* kDefaultProfilesPath = "profiles.json";
constexpr int kMinFirstRunIntervalS = 5;
constexpr int kMaxFirstRunIntervalS = 3600;
constexpr int kMinPollMs = 50;
constexpr int kMaxPollMs = 60000;
constexpr int kMaxModuleSeconds = 86400;

std::string stringOr(std::string_view key, const std::string& fallback)
{
  auto value = ConfigService::getString(std::string(key));
  return value.empty() ? fallback : value;
}

std::chrono::seconds secondsOr(const std::string& key, std::chrono::seconds fallback)
{
  const int value = ConfigService::getInt(key);
  if (value <= 0 || value > kMaxModuleSeconds)
    return fallback;
  return std::chrono::seconds(value);
}

std::chrono::milliseconds timeoutOr(const std::string& key, std::chrono::milliseconds fallback)
{
  const int value = ConfigService::getInt(key);
  if (value <= 0 || value > kMaxTimeoutMs)
    return fallback;
  return std::chrono::milliseconds(value);
}
}

ListenerConfig SettingsConfig::resolveListener()
{
  return ListenerConfig::resolveServiceTls("settings", 7045);
}

std::vector<SettingsOwnerConfig> SettingsConfig::resolveOwners()
{
  std::vector<SettingsOwnerConfig> owners;
  owners.reserve(kSettingsOwnerOrder.size());
  for (const auto name : kSettingsOwnerOrder) {
    const std::string prefix = "owners." + std::string(name) + ".";
    SettingsOwnerConfig owner{.name = std::string(name),
                              .target = ConfigService::getString(prefix + "target"),
                              .credential = ConfigService::getString(prefix + "credential"),
                              .configFile = ConfigService::getString(prefix + "config_file")};
    if (owner.target.empty())
      continue;
    if (owner.credential.empty()) {
      LOG_WARN << "Settings owner " << owner.name << " has a target but no credential; it stays unconfigured";
      continue;
    }
    owners.push_back(std::move(owner));
  }
  return owners;
}

std::vector<std::string> SettingsConfig::unconfiguredOwners(const std::vector<SettingsOwnerConfig>& owners)
{
  std::vector<std::string> names;
  for (const auto name : kSettingsOwnerOrder)
    if (std::ranges::find(owners, name, &SettingsOwnerConfig::name) == owners.end())
      names.emplace_back(name);
  return names;
}

FirstRunConfig SettingsConfig::resolveFirstRun()
{
  const FirstRunConfig defaults;
  const int interval = ConfigService::getInt("settings.first_run_interval_s");
  return {.enabled = !ConfigService::hasKey("settings.first_run") || ConfigService::getBool("settings.first_run"),
          .interval = interval >= kMinFirstRunIntervalS && interval <= kMaxFirstRunIntervalS
                          ? std::chrono::seconds(interval)
                          : defaults.interval};
}

SettingsTimeouts SettingsConfig::resolveTimeouts()
{
  const SettingsTimeouts defaults;
  return {.list = timeoutOr("settings.list_timeout_ms", defaults.list),
          .update = timeoutOr("settings.update_timeout_ms", defaults.update)};
}

std::string SettingsConfig::resolveProfilesPath()
{
  auto path = ConfigService::getString("settings.profiles_path");
  return path.empty() ? std::string(kDefaultProfilesPath) : path;
}

ModulesConfig SettingsConfig::resolveModules()
{
  const ModulesConfig defaults;
  const int poll = ConfigService::getInt("modules.poll_interval_ms");
  return {.catalogPath = stringOr("modules.catalog_path", defaults.catalogPath),
          .dbPath = stringOr("modules.db_path", defaults.dbPath),
          .schemaPath = stringOr("modules.schema", defaults.schemaPath),
          .modelsDir = stringOr("modules.models_dir", defaults.modelsDir),
          .pollInterval =
              poll >= kMinPollMs && poll <= kMaxPollMs ? std::chrono::milliseconds(poll) : defaults.pollInterval,
          .idleRefresh = secondsOr("modules.idle_refresh_s", defaults.idleRefresh),
          .healthTimeout = secondsOr("modules.health_timeout_s", defaults.healthTimeout),
          .seedWait = secondsOr("modules.seed_wait_s", defaults.seedWait)};
}

ModulesRpcConfig SettingsConfig::resolveModulesRpc()
{
  ModulesRpcConfig config{.address = ConfigService::getString("rpc.address"), .callers = {}};
  for (auto& [caller, secret] : ConfigService::getStringPairs("rpc.callers"))
    if (std::ranges::find(kModuleStateCallers, caller) != kModuleStateCallers.end())
      config.callers.emplace_back(std::move(caller), std::move(secret));
  return config;
}
