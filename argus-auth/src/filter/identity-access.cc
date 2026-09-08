#include "identity-access.hxx"

#include <identity/identity-client.hxx>
#include <mutex>
#include <shared/services/config-service/config-service.hxx>
#include <string>

namespace
{

// identity.target when set, else the gateway's own listener keys — so the
// gateway needs no target of its own and every other service names one.
std::string resolveTarget()
{
  auto target = ConfigService::getString("identity.target");
  if (!target.empty())
    return target;

  auto host = ConfigService::getString("identity.rpc_host");
  const int port = ConfigService::getInt("identity.rpc_port");
  if (host.empty())
    host = "127.0.0.1";
  return host + ":" + std::to_string(port > 0 ? port : 7040);
}

} // namespace

std::shared_ptr<const IdentityClient> filterIdentityClient()
{
  static std::mutex mutex;
  static std::string cachedTarget;
  static std::string cachedSecret;
  static std::shared_ptr<const IdentityClient> client;

  const auto target = resolveTarget();
  const auto secret = ConfigService::getString("identity.rpc_secret");
  const std::lock_guard<std::mutex> lock(mutex);
  if (!client || target != cachedTarget || secret != cachedSecret) {
    cachedTarget = target;
    cachedSecret = secret;
    client = std::make_shared<IdentityClient>(target, secret);
  }
  return client;
}
