#include "identity-access.hxx"

#include <identity/identity-client.hxx>
#include <mutex>
#include <config/config-service.hxx>
#include <stdexcept>
#include <string>

namespace
{

std::string resolveTarget()
{
  auto target = ConfigService::getString("identity.target");
  if (target.empty())
    throw std::runtime_error("identity.target is empty; run scripts/setup.sh");
  return target;
}

}

void requireIdentityTarget()
{
  static_cast<void>(resolveTarget());
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
