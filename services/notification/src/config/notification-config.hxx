#pragma once

#include <http/listener-config.hxx>

#include <string>

struct NotificationDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct NotificationIdentityConfig
{
  std::string target;
  std::string rpcSecret;
};

class NotificationConfig
{
public:
  static NotificationDbConfig resolveDb();
  static ListenerConfig resolveListener();
  static GrpcListenerConfig resolveRpcListener();
  static NotificationIdentityConfig resolveIdentity();
};
