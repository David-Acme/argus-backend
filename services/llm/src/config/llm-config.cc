#include "llm-config.hxx"

#include <config/config-service.hxx>
#include <grpc/fleet-caller-gate.hxx>
#include <settings/settings-rpc.hxx>

#include <algorithm>
#include <array>
#include <utility>

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
          .credential = ConfigService::getString("identity.credential"),
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

std::optional<LlmDecisionConfig> LlmConfig::resolveDecision(std::string_view decider)
{
  const std::string section = decider.empty() ? std::string("decide") : "decide." + std::string(decider);
  if (!ConfigService::hasKey(section + ".act") || !ConfigService::hasKey(section + ".ask"))
    return std::nullopt;
  const LlmDecisionConfig decision{.act = ConfigService::getDouble(section + ".act"),
                                   .ask = ConfigService::getDouble(section + ".ask"),
                                   .margin = ConfigService::hasKey(section + ".margin") ? ConfigService::getDouble(section + ".margin") : 0.0};
  if (decision.ask <= 0.0 || decision.ask > decision.act || decision.act > 1.0 || decision.margin < 0.0 || decision.margin > 1.0)
    return std::nullopt;
  return decision;
}

bool LlmConfig::resolveWitnessOnly(std::string_view decider)
{
  const std::string key = "decide." + std::string(decider) + ".witness_only";
  return ConfigService::hasKey(key) && ConfigService::getBool(key);
}

LlmNotificationConfig LlmConfig::resolveNotifications()
{
  return {.target = ConfigService::getString("notifications.target"),
          .credential = ConfigService::getString("notifications.credential")};
}

std::filesystem::path LlmConfig::resolveComponentsRoot()
{
  const std::string root = ConfigService::getString("components.models_dir");
  return root.empty() ? std::filesystem::path("models") : std::filesystem::path(root);
}

std::vector<LlmToolProviderConfig> LlmConfig::resolveToolProviders()
{
  struct Source
  {
    const char* id;
    const char* targetKey;
    const char* credentialKey;
  };
  constexpr std::array<Source, 4> kSources{{{.id = "camera", .targetKey = "camera.grpc_target", .credentialKey = "camera.credential"},
                                            {.id = "guard", .targetKey = "guard.target", .credentialKey = "guard.credential"},
                                            {.id = "productivity",
                                             .targetKey = "productivity.grpc_target",
                                             .credentialKey = "productivity.credential"},
                                            {.id = "settings", .targetKey = "modules.target", .credentialKey = "modules.credential"}}};
  std::vector<LlmToolProviderConfig> providers;
  for (const auto& source : kSources) {
    LlmToolProviderConfig provider{.id = source.id,
                                   .target = ConfigService::getString(source.targetKey),
                                   .credential = ConfigService::getString(source.credentialKey)};
    if (!provider.target.empty() && argus::client::FleetCallerGate::pairedSecret(provider.credential))
      providers.push_back(std::move(provider));
  }
  return providers;
}
