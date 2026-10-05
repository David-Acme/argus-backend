#include "identity-access.hxx"

#include <identity/identity-client.hxx>
#include <mutex>
#include <config/config-service.hxx>
#include <string>

namespace
{

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

}

std::shared_ptr<const IdentityClient> filterIdentityClient()
{
  static std::mutex mutex;
  static std::string cachedTarget;
  static std::string cachedCredential;
  static std::string cachedSecret;
  static std::shared_ptr<const IdentityClient> client;

  const auto target = resolveTarget();
  const auto credential = ConfigService::getString("identity.credential");
  const auto secret = ConfigService::getString("identity.rpc_secret");
  const std::scoped_lock lock(mutex);
  if (!client || target != cachedTarget || credential != cachedCredential ||
      secret != cachedSecret) {
    cachedTarget = target;
    cachedCredential = credential;
    cachedSecret = secret;
    client = std::make_shared<IdentityClient>(
        target, argus::client::PeerCredential{.credential = credential,
                                              .fleetSecret = secret});
  }
  return client;
}
