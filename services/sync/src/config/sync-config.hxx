#pragma once

#include <http/listener-config.hxx>
#include <string>

struct SyncDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct SyncControlConfig
{
  GrpcListenerConfig listener;
  std::string secret;

  [[nodiscard]] bool reachableBeyondLoopback() const;
};

struct SyncUpstreams
{
  std::string camera;
  std::string productivity;
  std::string notification;
  std::string identity;
  std::string identitySecret;
};

class SyncConfig
{
public:
  static SyncDbConfig resolveDb();

  static ListenerConfig resolveListener();

  static SyncControlConfig resolveControl();
  static SyncUpstreams resolveUpstreams();
  static int resolveAuditRetentionDays();
};
