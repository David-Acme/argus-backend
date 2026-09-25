#pragma once

#include <http/listener-config.hxx>
#include <shared/vocabulary/health-thresholds.hxx>

#include <cstdint>
#include <string>

struct CameraDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct CameraHealthConfig
{
  bool enabled{true};
  int64_t intervalMs{60000};
  HealthThresholds thresholds;
};

class CameraConfig
{
public:
  static CameraDbConfig resolveDb();
  static ListenerConfig resolveListener();
  static CameraHealthConfig resolveHealth();
  static std::string resolveGuardCallerSecret();
};
