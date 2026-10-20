#include "llm-config.hxx"

#include <config/config-service.hxx>
#include <grpc/fleet-caller-gate.hxx>
#include <settings/settings-rpc.hxx>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace
{
constexpr std::string_view kDecideBundle = "models/decide/bundle-laya-mvp";
constexpr std::string_view kExtractBundle = "models/extract/bundle-gliner-pilot";
}

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

MessengerSettings LlmConfig::resolveMessenger()
{
  MessengerSettings settings;
  if (ConfigService::hasKey("messenger.enabled"))
    settings.enabled = ConfigService::getBool("messenger.enabled");
  if (ConfigService::hasKey("messenger.reply_window_s"))
    settings.replyWindowSeconds = ConfigService::getInt("messenger.reply_window_s");
  if (ConfigService::hasKey("messenger.max_relays_per_hour"))
    settings.maxRelaysPerHour = ConfigService::getInt("messenger.max_relays_per_hour");
  if (ConfigService::hasKey("messenger.max_message_chars"))
    settings.maxMessageChars = ConfigService::getInt("messenger.max_message_chars");
  if (ConfigService::hasKey("messenger.max_visitor_notices_per_hour_per_camera"))
    settings.maxVisitorNoticesPerHourPerCamera = ConfigService::getInt("messenger.max_visitor_notices_per_hour_per_camera");
  settings.retentionDays = resolveConversation().retentionDays;
  return settings;
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

LlmConversationConfig LlmConfig::resolveConversation()
{
  LlmConversationConfig config;
  if (ConfigService::hasKey("conversation.idle_seconds"))
    config.idleSeconds = ConfigService::getInt("conversation.idle_seconds");
  if (ConfigService::hasKey("conversation.retention_days"))
    config.retentionDays = ConfigService::getInt("conversation.retention_days");
  if (ConfigService::hasKey("conversation.switch_budget_ms"))
    config.switchBudgetMs = ConfigService::getInt("conversation.switch_budget_ms");
  return boundConversation(config);
}

LlmConversationConfig LlmConfig::boundConversation(LlmConversationConfig config)
{
  config.idleSeconds = std::clamp<int64_t>(config.idleSeconds, 60, 86400);
  config.retentionDays = std::clamp<int64_t>(config.retentionDays, 1, 365);
  config.switchBudgetMs = std::clamp<int64_t>(config.switchBudgetMs, 1, 1000);
  return config;
}

LlmContextConfig LlmConfig::resolveContext()
{
  LlmContextConfig config;
  if (ConfigService::hasKey("context.budget_ms"))
    config.budgetMs = ConfigService::getInt("context.budget_ms");
  if (ConfigService::hasKey("context.max_lines"))
    config.maxLines = ConfigService::getInt("context.max_lines");
  if (ConfigService::hasKey("context.max_chars"))
    config.maxChars = ConfigService::getInt("context.max_chars");
  if (ConfigService::hasKey("context.agenda_days"))
    config.agendaDays = ConfigService::getInt("context.agenda_days");
  return boundContext(config);
}

LlmContextConfig LlmConfig::boundContext(LlmContextConfig config)
{
  config.budgetMs = std::clamp<int64_t>(config.budgetMs, 0, 5000);
  config.maxLines = std::clamp<int64_t>(config.maxLines, 0, 10);
  config.maxChars = std::clamp<int64_t>(config.maxChars, 40, 600);
  config.agendaDays = std::clamp<int64_t>(config.agendaDays, 0, 14);
  return config;
}

std::optional<LlmDecisionConfig> LlmConfig::resolveDecision(std::string_view decider){
  const std::string section = decider.empty() ? std::string("decide") : "decide." + std::string(decider);
  if (!ConfigService::hasKey(section + ".act") || !ConfigService::hasKey(section + ".ask"))
    return std::nullopt;
  const LlmDecisionConfig decision{.act = ConfigService::getDouble(section + ".act"),
                                   .ask = ConfigService::getDouble(section + ".ask"),
                                   .margin = ConfigService::hasKey(section + ".margin") ? ConfigService::getDouble(section + ".margin") : 0.0,
                                   .nowMin = ConfigService::hasKey(section + ".now_min") ? ConfigService::getDouble(section + ".now_min") : 0.0};
  if (decision.ask <= 0.0 || decision.ask > decision.act || decision.act > 1.0 || decision.margin < 0.0 || decision.margin > 1.0 ||
      decision.nowMin < 0.0 || decision.nowMin > 1.0)
    return std::nullopt;
  return decision;
}

bool LlmConfig::resolveWitnessOnly(std::string_view decider)
{
  const std::string key = "decide." + std::string(decider) + ".witness_only";
  return ConfigService::hasKey(key) && ConfigService::getBool(key);
}

LlmBundleConfig LlmConfig::resolveDecideBundle()
{
  const std::string dir = ConfigService::getString("decide.bundle_dir");
  return {.dir = dir.empty() ? std::filesystem::path(kDecideBundle) : std::filesystem::path(dir)};
}

LlmBundleConfig LlmConfig::resolveExtractBundle()
{
  const std::string dir = ConfigService::getString("extract.bundle_dir");
  return {.dir = dir.empty() ? std::filesystem::path(kExtractBundle) : std::filesystem::path(dir)};
}

LlmOnnxConfig LlmConfig::resolveOnnx(std::string_view section)
{
  const std::string prefix(section);
  LlmOnnxConfig config;
  if (ConfigService::hasKey(prefix + ".threads"))
    config.threads = ConfigService::getInt(prefix + ".threads");
  if (ConfigService::hasKey(prefix + ".cpu_arena"))
    config.cpuArena = ConfigService::getBool(prefix + ".cpu_arena");
  if (ConfigService::hasKey(prefix + ".mem_pattern"))
    config.memPattern = ConfigService::getBool(prefix + ".mem_pattern");
  if (ConfigService::hasKey(prefix + ".prepacking"))
    config.prepacking = ConfigService::getBool(prefix + ".prepacking");
  if (ConfigService::hasKey(prefix + ".mmap"))
    config.mmap = ConfigService::getBool(prefix + ".mmap");
  return config;
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
