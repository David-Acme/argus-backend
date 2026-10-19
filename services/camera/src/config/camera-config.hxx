#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>
#include <shared/vocabulary/health-thresholds.hxx>

#include <cstdint>
#include <string>
#include <vector>

struct CameraDbConfig
{
  std::string dbPath;
  std::string schemaPath;
  std::string secretKeyPath;
};

struct CameraHealthConfig
{
  bool enabled{true};
  int64_t intervalMs{60000};
  HealthThresholds thresholds;
  int64_t rebaselineAfterMs{600000};
  int64_t protectIntervalMs{5000};
  int64_t dayNightHoldMs{30000};
};

struct CameraPtzConfig
{
  int64_t parkMs{60000};
  int64_t idleReleaseMs{60000};
  int64_t settleMs{4000};
  int64_t returnAfterLookMs{10000};
  int64_t returnAfterFollowMs{30000};
  int64_t calibrateEveryMs{86400000};
  int64_t dailyMoveCap{400};
  int movesPerMinute{6};
  double deadZone{0.05};
  double fovHorizontalDeg{77.0};
  double fovVerticalDeg{43.0};
  int64_t lookBudgetMs{8000};
  int64_t followLostMs{10000};
  int leadMinSamples{3};
  double leadConsistency{0.7};
  bool patrolEnabled{true};
  int64_t patrolEveryMs{600000};
  int64_t patrolDwellMs{30000};
};

struct CameraLookConfig
{
  bool enabled{true};
  double doubtMinConfidence{0.25};
  double smallBoxShare{0.03};
  int grabs{3};
  int64_t budgetMs{3000};
  double cropScale{2.0};
  double confirmConfidence{0.45};
  int64_t cooldownMs{60000};
};

struct CameraWatchConfig
{
  int64_t micFrameMs{32};
  int64_t micReconnectMs{1000};
  int64_t micReconnectMaxMs{10000};
  int64_t micLingerMs{2000};
  int64_t gateRecheckMs{1000};
  int64_t micQueueFrames{64};
  int64_t earconMs{300};
  int64_t leadSilenceMs{700};
  int64_t tailPadMs{300};
  int64_t speakQueueMs{60000};
  int64_t speakIdleMs{15000};
  int64_t packetMs{120};
  int64_t maxListeners{16};
  int64_t boardRefreshMs{2000};
  int64_t faceWindowMaxMs{3000};
  int64_t faceWindowMaxCrops{6};
  int64_t sightingHoldMs{45000};
  int64_t aimInFrameMs{3000};
  int64_t aimHorizontalFovDegrees{100};
  int64_t aimVerticalFovDegrees{56};
  int64_t aimDeadZonePercent{5};
  int64_t aimSettleMs{1500};
  int64_t aimLookMs{2000};
  int64_t aimMaxPresets{4};
  int64_t aimReturnMs{30000};
};

struct GuardDutyConfig
{
  std::string target;
  std::string credential;
};

struct CameraOnvifConfig
{
  bool events{true};
};

struct PrivacyNoticeConfig
{
  std::string identityTarget;
  std::string identityCredential;
  std::string identityFleetSecret;
  std::string notificationTarget;
  std::string notificationCredential;
};

class CameraConfig
{
public:
  static CameraDbConfig resolveDb();
  static ListenerConfig resolveListener();
  static CameraHealthConfig resolveHealth();
  static CameraOnvifConfig resolveOnvif();
  static CameraPtzConfig resolvePtz();
  static CameraLookConfig resolveLook();
  static CameraWatchConfig resolveWatch();
  static PrivacyNoticeConfig resolvePrivacyNotice();
  static GuardDutyConfig resolveGuardDuty();
  static std::string resolveGuardCallerSecret();
  static std::string resolveSyncCallerSecret();
  static std::string resolveLlmCallerSecret();
  static std::string resolveVoiceCallerSecret();
  static std::vector<argus::client::CallerCredential> resolveSettingsCallers();
};
