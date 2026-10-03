#pragma once

#include <grpc/grpc-server-identity.hxx>
#include <http/listener-config.hxx>

#include <cstdint>
#include <string>
#include <vector>

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
  static std::vector<argus::client::CallerCredential> resolveSettingsCallers();
  static int64_t resolveAckWindowS();
  static int64_t resolveSelfTestIntervalS();
};
