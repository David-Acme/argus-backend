#include "llm-config.hxx"

#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

#include <algorithm>

ListenerConfig LlmConfig::resolveListener()
{
  return ListenerConfig::resolve(7032);
}

LlmRpcConfig LlmConfig::resolveRpc()
{
  LlmRpcConfig config;
  config.address = ConfigService::getString("rpc.address");
  config.credentials = ConfigService::getStringPairs("rpc.callers");
  std::erase_if(config.credentials, [](const auto& credential) {
    return credential.first.empty() || credential.second.empty();
  });
  config.settingsCredentials = settingsCallers(config.credentials);
  withoutSettingsCaller(config.credentials);
  return config;
}

LlmIdentityConfig LlmConfig::resolveIdentity()
{
  return {.target = ConfigService::getString("identity.target"),
          .rpcSecret = ConfigService::getString("identity.rpc_secret")};
}

std::string LlmConfig::resolveCameraTarget()
{
  return ConfigService::getString("camera.grpc_target");
}

std::string LlmConfig::resolveCameraCredential()
{
  return ConfigService::getString("camera.credential");
}

LlmMemoryConfig LlmConfig::resolveMemory()
{
  return {.observeCameraEvents =
              ConfigService::getBool("memory.observe_camera_events")};
}

LlmNotificationConfig LlmConfig::resolveNotifications()
{
  return {.target = ConfigService::getString("notifications.target"),
          .credential = ConfigService::getString("notifications.credential")};
}
