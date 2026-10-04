#include "auth-config.hxx"

#include <config/config-service.hxx>

namespace
{
constexpr int64_t kDefaultContextCacheSeconds = 30;
constexpr int64_t kDefaultRefreshReuseGraceSeconds = 30;
constexpr int kDefaultIdentityPort = 7040;
constexpr uint16_t kDefaultAuthPort = 7042;
constexpr uint16_t kDefaultRpcPort = 7043;
constexpr int kDefaultRateLimitWindowSeconds = 60;
constexpr int kDefaultRateLimitMaxRequests = 10;
constexpr int kDefaultRateLimitLockoutThreshold = 5;
constexpr int kDefaultRateLimitLockoutSeconds = 300;

int positiveOr(int value, int fallback)
{
  return value > 0 ? value : fallback;
}
}

AuthDbConfig AuthConfig::resolveDb()
{
  AuthDbConfig config;
  config.dbPath = ConfigService::getString("auth.db");
  if (config.dbPath.empty())
    config.dbPath = "database/auth.db";
  config.schemaPath = ConfigService::getString("auth.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/auth/database/schema.sql";
  return config;
}

ListenerConfig AuthConfig::resolveListener()
{
  return ListenerConfig::resolveServiceTls("auth", kDefaultAuthPort);
}

AuthRpcConfig AuthConfig::resolveRpc()
{
  AuthRpcConfig config;
  config.listener = GrpcListenerConfig::resolve(kDefaultRpcPort);
  config.secret = ConfigService::getString("auth.rpc_secret");
  return config;
}

bool AuthRpcConfig::reachableBeyondLoopback() const
{
  return listener.host != "127.0.0.1" && listener.host != "::1" &&
         listener.host != "localhost";
}

AuthIdentityConfig AuthConfig::resolveIdentity()
{
  AuthIdentityConfig config;
  config.target = ConfigService::getString("identity.target");
  if (config.target.empty()) {
    std::string host = ConfigService::getString("identity.rpc_host");
    if (host.empty())
      host = "127.0.0.1";
    const int port = ConfigService::getInt("identity.rpc_port");
    config.target =
        host + ":" +
        std::to_string(port > 0 ? port : kDefaultIdentityPort);
  }
  config.secret = ConfigService::getString("identity.rpc_secret");
  return config;
}

AuthRateLimitConfig AuthConfig::resolveRateLimit()
{
  AuthRateLimitConfig config;
  config.enabled = !ConfigService::hasKey("rate_limit.enabled") ||
                   ConfigService::getBool("rate_limit.enabled");
  config.windowSeconds =
      positiveOr(ConfigService::getInt("rate_limit.window_seconds"),
                 kDefaultRateLimitWindowSeconds);
  config.maxRequests =
      positiveOr(ConfigService::getInt("rate_limit.max_requests"),
                 kDefaultRateLimitMaxRequests);
  config.lockoutThreshold =
      positiveOr(ConfigService::getInt("rate_limit.lockout_threshold"),
                 kDefaultRateLimitLockoutThreshold);
  config.lockoutSeconds =
      positiveOr(ConfigService::getInt("rate_limit.lockout_seconds"),
                 kDefaultRateLimitLockoutSeconds);
  return config;
}

int64_t AuthConfig::resolveContextCacheSeconds()
{
  if (!ConfigService::hasKey("auth.context_cache_seconds"))
    return kDefaultContextCacheSeconds;
  const int64_t seconds = ConfigService::getInt("auth.context_cache_seconds");
  return seconds >= 0 ? seconds : kDefaultContextCacheSeconds;
}

int64_t AuthConfig::resolveRefreshReuseGraceSeconds()
{
  if (!ConfigService::hasKey("auth.refresh_reuse_grace_seconds"))
    return kDefaultRefreshReuseGraceSeconds;
  const int64_t seconds =
      ConfigService::getInt("auth.refresh_reuse_grace_seconds");
  return seconds >= 0 ? seconds : kDefaultRefreshReuseGraceSeconds;
}
