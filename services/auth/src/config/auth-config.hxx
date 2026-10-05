#pragma once

#include <cstdint>
#include <http/listener-config.hxx>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct AuthDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct AuthRpcConfig
{
  GrpcListenerConfig listener;
  std::string secret;
  std::vector<std::pair<std::string, std::string>> callers;

  [[nodiscard]] bool reachableBeyondLoopback() const;

  [[nodiscard]] std::optional<std::string> secretProblem() const;

  static constexpr std::size_t kMinSecretLength = 32;
};

struct AuthIdentityConfig
{
  std::string target;
  std::string credential;
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

class AuthConfig
{
public:
  [[nodiscard]] static AuthDbConfig resolveDb();

  [[nodiscard]] static ListenerConfig resolveListener();

  [[nodiscard]] static AuthRpcConfig resolveRpc();

  [[nodiscard]] static AuthIdentityConfig resolveIdentity();

  [[nodiscard]] static AuthRateLimitConfig resolveRateLimit();

  [[nodiscard]] static int64_t resolveContextCacheSeconds();

  [[nodiscard]] static int64_t resolveRefreshReuseGraceSeconds();
};
