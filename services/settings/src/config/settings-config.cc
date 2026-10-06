#include "settings-config.hxx"

#include <config/config-service.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <utility>

namespace
{
constexpr int kMaxTimeoutMs = 120000;
constexpr const char* kDefaultProfilesPath = "profiles.json";
constexpr int kMinFirstRunIntervalS = 5;
constexpr int kMaxFirstRunIntervalS = 3600;

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
