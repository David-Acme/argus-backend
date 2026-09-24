#include "auth-access.hxx"

#include <auth/auth-client.hxx>
#include <config/config-service.hxx>
#include <mutex>
#include <string>

namespace
{

std::string resolveTarget()
{
  auto target = ConfigService::getString("auth.target");
  if (!target.empty())
    return target;

  auto host = ConfigService::getString("auth.rpc_host");
  const int port = ConfigService::getInt("auth.rpc_port");
  if (host.empty())
    host = "127.0.0.1";
  return host + ":" + std::to_string(port > 0 ? port : 7043);
}

}

std::shared_ptr<const AuthClient> filterAuthClient()
{
  static std::mutex mutex;
  static std::string cachedTarget;
  static std::string cachedSecret;
  static std::shared_ptr<const AuthClient> client;

  const auto target = resolveTarget();
  const auto secret = ConfigService::getString("auth.rpc_secret");
  const std::scoped_lock lock(mutex);
  if (!client || target != cachedTarget || secret != cachedSecret) {
    cachedTarget = target;
    cachedSecret = secret;
    client = std::make_shared<AuthClient>(
        AuthClientConfig{.target = target, .fleetSecret = secret});
  }
  return client;
}
