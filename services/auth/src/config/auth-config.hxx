#pragma once

#include <cstdint>
#include <http/listener-config.hxx>
#include <string>

struct AuthDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct AuthRpcConfig
{
  GrpcListenerConfig listener;
  std::string secret;

  [[nodiscard]] bool reachableBeyondLoopback() const;
};

struct AuthIdentityConfig
{
  std::string target;
  std::string secret;
};

struct AuthRateLimitConfig
{
  bool enabled{false};
  int windowSeconds{60};
  int maxRequests{10};
  int lockoutThreshold{5};
  int lockoutSeconds{300};
};

struct AuthSyncControlConfig
{
  std::string target;
  std::string secret;
};

class AuthConfig
{
public:
  [[nodiscard]] static AuthDbConfig resolveDb();

  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static AuthRpcConfig resolveRpc();

  [[nodiscard]] static AuthIdentityConfig resolveIdentity();

  [[nodiscard]] static AuthRateLimitConfig resolveRateLimit();

  [[nodiscard]] static AuthSyncControlConfig resolveSyncControl();

  [[nodiscard]] static int64_t resolveContextCacheSeconds();
};
