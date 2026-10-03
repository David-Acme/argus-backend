#pragma once

#include <cstdint>
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

struct IdentityRetentionConfig
{
  int64_t candidateDays{30};
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

  [[nodiscard]] static IdentityRetentionConfig resolveRetention();
};
