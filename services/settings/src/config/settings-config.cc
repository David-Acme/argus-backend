#include "settings-config.hxx"

#include <config/config-service.hxx>
#include <trantor/utils/Logger.h>

#include <utility>

namespace
{
constexpr int kMaxTimeoutMs = 120000;

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
                              .credential = ConfigService::getString(prefix + "credential")};
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

SettingsTimeouts SettingsConfig::resolveTimeouts()
{
  const SettingsTimeouts defaults;
  return {.list = timeoutOr("settings.list_timeout_ms", defaults.list),
          .update = timeoutOr("settings.update_timeout_ms", defaults.update)};
}
