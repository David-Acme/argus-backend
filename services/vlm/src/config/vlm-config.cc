#include "vlm-config.hxx"

#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

#include <algorithm>

ListenerConfig VlmConfig::resolveListener()
{
  return ListenerConfig::resolve(7031);
}

VlmRpcConfig VlmConfig::resolveRpc()
{
  VlmRpcConfig config;
  config.address = ConfigService::getString("rpc.address");
  config.credentials = ConfigService::getStringPairs("rpc.callers");
  std::erase_if(config.credentials, [](const auto& credential) {
    return credential.first.empty() || credential.second.empty();
  });
  config.settingsCredentials = settingsCallers(config.credentials);
  withoutSettingsCaller(config.credentials);
  return config;
}

std::filesystem::path VlmConfig::resolveComponentsRoot()
{
  const std::string root = ConfigService::getString("components.models_dir");
  return root.empty() ? std::filesystem::path("models") : std::filesystem::path(root);
}
