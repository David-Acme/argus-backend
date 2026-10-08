#include "identity-config.hxx"

#include <algorithm>
#include <cmath>
#include <config/config-service.hxx>
#include <cstdint>
#include <trantor/utils/Logger.h>

namespace
{
constexpr uint16_t kDefaultIdentityPort = 7044;
constexpr uint16_t kDefaultRpcPort = 7040;
constexpr int kMinLinkOccasions = 2;
constexpr int kMaxLinkOccasions = 20;
constexpr int kMinProfileSamples = 5;
constexpr int kMaxProfileSamples = 200;
constexpr int64_t kSecondsPerMinute = 60;
constexpr int64_t kSecondsPerDay = 86400;

struct UnitScoreInput
{
  const char* key{nullptr};
  float fallback{0.0F};
};

float unitScore(const UnitScoreInput& input)
{
  if (!ConfigService::hasKey(input.key))
    return input.fallback;
  return std::clamp(static_cast<float>(ConfigService::getDouble(input.key)),
                    0.0F, 1.0F);
}

float unitScoreBelow(const UnitScoreInput& input, float bound)
{
  const float value = unitScore(input);
  if (value < bound)
    return value;
  const float clamped = std::nextafter(bound, 0.0F);
  LOG_WARN << "identity: " << input.key << " " << value << " must stay below "
           << bound << "; clamping to " << clamped;
  return clamped;
}

float nonNegative(const char* key, float fallback)
{
  if (!ConfigService::hasKey(key))
    return fallback;
  return std::max(0.0F, static_cast<float>(ConfigService::getDouble(key)));
}
}

IdentityDbConfig IdentityConfig::resolveDb()
{
  IdentityDbConfig config;
  config.dbPath = ConfigService::getString("identity.db");
  if (config.dbPath.empty())
    config.dbPath = "database/identity.db";
  config.schemaPath = ConfigService::getString("identity.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/identity/database/schema.sql";
  return config;
}

ListenerConfig IdentityConfig::resolveListener()
{
  return ListenerConfig::resolveServiceTls("identity", kDefaultIdentityPort);
}

uint16_t IdentityConfig::resolveAnnouncedPort()
{
  return resolveListener().port;
}

IdentityRpcConfig IdentityConfig::resolveRpc()
{
  IdentityRpcConfig config;
  config.listener = GrpcListenerConfig::resolve(kDefaultRpcPort);
  config.secret = ConfigService::getString("identity.rpc_secret");
  config.callers = ConfigService::getStringPairs("rpc.callers");
  return config;
}

bool IdentityRpcConfig::reachableBeyondLoopback() const
{
  return listener.host != "127.0.0.1" && listener.host != "::1" &&
         listener.host != "localhost";
}

IdentitySyncControlConfig IdentityConfig::resolveSyncControl()
{
  IdentitySyncControlConfig config;
  config.target = ConfigService::getString("sync.control_target");
  config.credential = ConfigService::getString("sync.control_credential");
  config.secret = ConfigService::getString("sync.control_secret");
  return config;
}

IdentityFaceConfig IdentityConfig::resolveFace()
{
  IdentityFaceConfig config;
  config.enabled = ConfigService::getBool("face.enabled");
  if (ConfigService::hasKey("face.liveness_required"))
    config.livenessRequired = ConfigService::getBool("face.liveness_required");
  if (ConfigService::hasKey("face.liveness_threshold"))
    config.livenessThreshold = std::clamp(
        static_cast<float>(ConfigService::getDouble("face.liveness_threshold")),
        IdentityFaceConfig::kMinLivenessThreshold,
        IdentityFaceConfig::kMaxLivenessThreshold);
  if (const std::string dir = ConfigService::getString("face.liveness_model_dir");
      !dir.empty())
    config.livenessModelDir = dir;
  if (ConfigService::hasKey("face.login_margin"))
    config.loginMargin = std::clamp(
        static_cast<float>(ConfigService::getDouble("face.login_margin")), 0.0F,
        IdentityFaceConfig::kMaxLoginMargin);
  return config;
}

IdentityVoiceprintConfig IdentityConfig::resolveVoiceprint()
{
  IdentityVoiceprintConfig config;
  if (ConfigService::hasKey("voiceprint.enabled"))
    config.enabled = ConfigService::getBool("voiceprint.enabled");
  if (const std::string model = ConfigService::getString("voiceprint.model");
      !model.empty())
    config.modelPath = model;
  config.identifyThreshold =
      unitScore({.key = "voiceprint.identify_threshold",
                 .fallback = config.identifyThreshold});
  config.identifyMargin = unitScore(
      {.key = "voiceprint.identify_margin", .fallback = config.identifyMargin});
  config.unfamiliarCeiling =
      unitScoreBelow({.key = "voiceprint.unfamiliar_ceiling",
                      .fallback = config.unfamiliarCeiling},
                     config.identifyThreshold);
  config.minVerifySpeechSeconds = nonNegative(
      "voiceprint.min_verify_speech_seconds", config.minVerifySpeechSeconds);
  config.minSnrDb = nonNegative("voiceprint.min_snr_db", config.minSnrDb);

  PassiveVoiceConfig& passive = config.passive;
  if (ConfigService::hasKey("voiceprint.passive_enabled"))
    passive.enabled = ConfigService::getBool("voiceprint.passive_enabled");
  if (ConfigService::hasKey("voiceprint.link_min_occasions"))
    passive.linkMinOccasions =
        std::clamp(ConfigService::getInt("voiceprint.link_min_occasions"),
                   kMinLinkOccasions, kMaxLinkOccasions);
  if (ConfigService::hasKey("voiceprint.link_min_days"))
    passive.linkMinDays = std::clamp(
        ConfigService::getInt("voiceprint.link_min_days"), 1,
        passive.linkMinOccasions);
  passive.linkDominance = std::max(
      0.5F, unitScore({.key = "voiceprint.link_dominance",
                       .fallback = passive.linkDominance}));
  if (ConfigService::hasKey("voiceprint.max_profile_samples"))
    passive.maxProfileSamples =
        std::clamp(ConfigService::getInt("voiceprint.max_profile_samples"),
                   kMinProfileSamples, kMaxProfileSamples);
  if (ConfigService::hasKey("voiceprint.window_days"))
    passive.windowSeconds =
        std::clamp<int64_t>(ConfigService::getInt("voiceprint.window_days"), 1,
                            365) *
        kSecondsPerDay;
  if (ConfigService::hasKey("voiceprint.call_idle_minutes"))
    passive.callIdleSeconds =
        std::clamp<int64_t>(ConfigService::getInt("voiceprint.call_idle_minutes"),
                            1, 60) *
        kSecondsPerMinute;
  return config;
}

IdentityRateLimitConfig IdentityConfig::resolveRateLimit()
{
  IdentityRateLimitConfig config;
  if (ConfigService::hasKey("rate_limit.enabled"))
    config.enabled = ConfigService::getBool("rate_limit.enabled");
  if (ConfigService::hasKey("rate_limit.window_seconds"))
    config.windowSeconds =
        std::clamp(ConfigService::getInt("rate_limit.window_seconds"), 1, 3600);
  if (ConfigService::hasKey("rate_limit.max_requests"))
    config.maxRequests =
        std::clamp(ConfigService::getInt("rate_limit.max_requests"), 1, 1000);
  if (ConfigService::hasKey("rate_limit.lockout_threshold"))
    config.lockoutThreshold =
        std::clamp(ConfigService::getInt("rate_limit.lockout_threshold"), 1, 100);
  if (ConfigService::hasKey("rate_limit.lockout_seconds"))
    config.lockoutSeconds =
        std::clamp(ConfigService::getInt("rate_limit.lockout_seconds"), 1, 86400);
  return config;
}

IdentityInvitationConfig IdentityConfig::resolveInvitation()
{
  IdentityInvitationConfig config;
  if (ConfigService::hasKey("invitation.lifetime_seconds"))
    config.lifetimeSeconds =
        std::clamp<int64_t>(ConfigService::getInt("invitation.lifetime_seconds"),
                            IdentityInvitationConfig::kMinLifetimeSeconds,
                            IdentityInvitationConfig::kMaxLifetimeSeconds);
  return config;
}
