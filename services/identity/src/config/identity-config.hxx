#pragma once

#include <cstdint>
#include <http/listener-config.hxx>
#include <string>

struct IdentityDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct IdentityRpcConfig
{
  GrpcListenerConfig listener;
  std::string secret;

  [[nodiscard]] bool reachableBeyondLoopback() const;
};

struct IdentitySyncControlConfig
{
  std::string target;
  std::string secret;
};

struct IdentityFaceConfig
{
  bool enabled{false};
};

struct IdentityRetentionConfig
{
  int64_t candidateDays{30};
};

struct IdentityVoiceprintConfig
{
  bool enabled{true};
  std::string modelPath{
      "models/speaker/3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx"};
  float verifyThreshold{0.50F};
  float identifyThreshold{0.55F};
  float identifyMargin{0.05F};
  float consistencyThreshold{0.50F};
  float minSpeechSeconds{1.2F};
  float minVerifySpeechSeconds{0.8F};
  float minSnrDb{12.0F};
  int samplesRequired{3};
  int64_t challengeSeconds{300};
};

class IdentityConfig
{
public:
  [[nodiscard]] static IdentityDbConfig resolveDb();

  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static uint16_t resolveAnnouncedPort();

  [[nodiscard]] static IdentityRpcConfig resolveRpc();

  [[nodiscard]] static IdentitySyncControlConfig resolveSyncControl();

  [[nodiscard]] static IdentityFaceConfig resolveFace();

  [[nodiscard]] static IdentityRetentionConfig resolveRetention();

  [[nodiscard]] static IdentityVoiceprintConfig resolveVoiceprint();
};
