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

class AuthConfig
{
public:
  [[nodiscard]] static AuthDbConfig resolveDb();

  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static AuthRpcConfig resolveRpc();

  [[nodiscard]] static AuthIdentityConfig resolveIdentity();

  [[nodiscard]] static int64_t resolveContextCacheSeconds();
};
