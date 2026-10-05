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

class CameraConfig
{
public:
  static CameraDbConfig resolveDb();
  static ListenerConfig resolveListener();
  static CameraHealthConfig resolveHealth();
  static std::string resolveGuardCallerSecret();
  static std::string resolveSyncCallerSecret();
  static std::string resolveLlmCallerSecret();
  static std::vector<argus::client::CallerCredential> resolveSettingsCallers();
};
