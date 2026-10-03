#include "identity-config.hxx"

#include <algorithm>
#include <config/config-service.hxx>
#include <cstdint>

namespace
{
constexpr uint16_t kDefaultIdentityPort = 7044;
constexpr uint16_t kDefaultRpcPort = 7040;
constexpr int kMinVoiceSamples = 3;
constexpr int kMaxVoiceSamples = 10;
constexpr int64_t kMinChallengeSeconds = 60;
constexpr int64_t kMaxChallengeSeconds = 1800;

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
  config.secret = ConfigService::getString("sync.control_secret");
  return config;
}

IdentityFaceConfig IdentityConfig::resolveFace()
{
  IdentityFaceConfig config;
  config.enabled = ConfigService::getBool("face.enabled");
  return config;
}

IdentityRetentionConfig IdentityConfig::resolveRetention()
{
  IdentityRetentionConfig config;
  if (ConfigService::hasKey("retention.candidate_days"))
    config.candidateDays =
        std::max(0, ConfigService::getInt("retention.candidate_days"));
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
  config.verifyThreshold = unitScore(
      {.key = "voiceprint.verify_threshold", .fallback = config.verifyThreshold});
  config.identifyThreshold =
      unitScore({.key = "voiceprint.identify_threshold",
                 .fallback = config.identifyThreshold});
  config.identifyMargin = unitScore(
      {.key = "voiceprint.identify_margin", .fallback = config.identifyMargin});
  config.consistencyThreshold =
      unitScore({.key = "voiceprint.consistency_threshold",
                 .fallback = config.consistencyThreshold});
  config.minSpeechSeconds =
      nonNegative("voiceprint.min_speech_seconds", config.minSpeechSeconds);
  config.minVerifySpeechSeconds = nonNegative(
      "voiceprint.min_verify_speech_seconds", config.minVerifySpeechSeconds);
  config.minSnrDb = nonNegative("voiceprint.min_snr_db", config.minSnrDb);
  if (ConfigService::hasKey("voiceprint.samples_required"))
    config.samplesRequired =
        std::clamp(ConfigService::getInt("voiceprint.samples_required"),
                   kMinVoiceSamples, kMaxVoiceSamples);
  if (ConfigService::hasKey("voiceprint.challenge_seconds"))
    config.challengeSeconds = std::clamp<int64_t>(
        ConfigService::getInt("voiceprint.challenge_seconds"),
        kMinChallengeSeconds, kMaxChallengeSeconds);
  return config;
}
