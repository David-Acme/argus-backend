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
  int64_t rebaselineAfterMs{900000};
};

struct CameraPtzConfig
{
  int64_t parkMs{60000};
  int64_t idleReleaseMs{60000};
  int64_t settleMs{4000};
  int64_t returnAfterLookMs{10000};
  int64_t returnAfterFollowMs{30000};
  int64_t calibrateEveryMs{86400000};
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
  static PrivacyNoticeConfig resolvePrivacyNotice();
  static GuardDutyConfig resolveGuardDuty();
  static std::string resolveGuardCallerSecret();
  static std::string resolveSyncCallerSecret();
  static std::string resolveLlmCallerSecret();
  static std::vector<argus::client::CallerCredential> resolveSettingsCallers();
};
