#pragma once

#include <cstdint>
#include <http/listener-config.hxx>
#include <string>
#include <utility>
#include <vector>

struct IdentityDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct IdentityRpcConfig
{
  GrpcListenerConfig listener;
  std::string secret;
  std::vector<std::pair<std::string, std::string>> callers;

  [[nodiscard]] bool reachableBeyondLoopback() const;
};

struct IdentitySyncControlConfig
{
  std::string target;
  std::string credential;
  std::string secret;
};

struct IdentityFaceConfig
{
  static constexpr float kMinLivenessThreshold = 0.50F;
  static constexpr float kMaxLivenessThreshold = 0.99F;
  static constexpr float kMaxLoginMargin = 0.30F;
  bool enabled{false};
  bool livenessRequired{true};
  float livenessThreshold{0.80F};
  std::string livenessModelDir{"models/face/anti-spoof"};
  float loginMargin{0.05F};
};

struct IdentityRateLimitConfig
{
  bool enabled{true};
  int windowSeconds{60};
  int maxRequests{10};
  int lockoutThreshold{5};
  int lockoutSeconds{300};
};

struct IdentityInvitationConfig
{
  static constexpr int64_t kMinLifetimeSeconds = 60;
  static constexpr int64_t kMaxLifetimeSeconds = 86400;
  int64_t lifetimeSeconds{1800};
};

struct PassiveVoiceConfig
{
  bool enabled{true};
  float minTurnSpeechSeconds{2.0F};
  float minTurnSnrDb{15.0F};
  float maxTurnClippedRatio{0.005F};
  float minHalfSeconds{0.9F};
  float turnSplitThreshold{0.15F};
  float callConsistency{0.55F};
  float otherSpeakerThreshold{0.50F};
  int minCallTurns{2};
  int maxCallTurns{12};
  float minCallSpeechSeconds{5.0F};
  int64_t callIdleSeconds{300};
  int maxOpenCalls{64};
  float clusterThreshold{0.60F};
  int linkMinOccasions{3};
  int linkMinDays{2};
  float linkDominance{0.75F};
  float linkMinSpeechSeconds{20.0F};
  int64_t occasionGapSeconds{3600};
  float adoptThreshold{0.60F};
  float sharedDeviceMargin{0.05F};
  float deviceConflictLimit{0.50F};
  int deviceConflictMinCalls{4};
  int refreshBatch{3};
  float refreshBlend{0.30F};
  float refreshMinAgreement{0.60F};
  float outlierFloor{0.45F};
  int maxProfileSamples{40};
  int maxPendingSamples{20};
  int64_t windowSeconds{int64_t{30} * 86400};
  int64_t adoptedRetentionSeconds{int64_t{180} * 86400};
};

struct IdentityVoiceprintConfig
{
  bool enabled{true};
  std::string modelPath{
      "models/speaker/3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx"};
  float identifyThreshold{0.55F};
  float identifyMargin{0.05F};
  float unfamiliarCeiling{0.40F};
  float minVerifySpeechSeconds{0.8F};
  float minSnrDb{12.0F};
  PassiveVoiceConfig passive;
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


  [[nodiscard]] static IdentityVoiceprintConfig resolveVoiceprint();

  [[nodiscard]] static IdentityInvitationConfig resolveInvitation();

  [[nodiscard]] static IdentityRateLimitConfig resolveRateLimit();
};
