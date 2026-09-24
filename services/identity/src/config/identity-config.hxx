#pragma once

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

class IdentityConfig
{
public:
  [[nodiscard]] static IdentityDbConfig resolveDb();

  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static IdentityRpcConfig resolveRpc();

  [[nodiscard]] static IdentitySyncControlConfig resolveSyncControl();

  [[nodiscard]] static IdentityFaceConfig resolveFace();
};
