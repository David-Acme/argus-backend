#pragma once

#include <chrono>
#include <cstdint>
#include <http/listener-config.hxx>
#include <string>
#include <utility>
#include <vector>

struct SyncDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct SyncControlConfig
{
  GrpcListenerConfig listener;
  std::string secret;
  std::vector<std::pair<std::string, std::string>> callers;

  [[nodiscard]] bool reachableBeyondLoopback() const;
};

struct SyncUpstreams
{
  std::string camera;
  std::string productivity;
  std::string notification;
  std::string identity;
  std::string identityCredential;
  std::string identitySecret;
};

struct SyncRtcConfig
{
  bool enabled{false};
  std::string apiKey;
  std::string apiSecret;
  std::string serverUrl;
  std::string publicUrl;
  uint16_t publicPort{7046};
  std::chrono::seconds tokenTtl{60};
  int maxConcurrentCalls{2};
};

struct SyncHeartbeatConfig
{
  int64_t intervalSeconds{60};
  int64_t graceSeconds{2700};
  int64_t socketGraceSeconds{180};
  int64_t guardStaleSeconds{90};
  int64_t pushIntervalSeconds{900};
  int64_t refillSeconds{300};
  std::string presenceTarget;
  std::string presenceCredential;
};

class SyncConfig
{
public:
  static SyncDbConfig resolveDb();

  static ListenerConfig resolveListener();

  static SyncControlConfig resolveControl();
  static SyncUpstreams resolveUpstreams();
  static int resolveAuditRetentionDays();
  static SyncRtcConfig resolveRtc();
  static SyncHeartbeatConfig resolveHeartbeat();
};
