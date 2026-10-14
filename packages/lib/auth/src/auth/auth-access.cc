#include "auth-access.hxx"

#include <auth/auth-client.hxx>
#include <config/config-service.hxx>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{

std::string resolveTarget()
{
  auto target = ConfigService::getString("auth.target");
  if (target.empty())
    throw std::runtime_error("auth.target is empty; run scripts/setup.sh");
  return target;
}

std::mutex& localMutex()
{
  static std::mutex mutex;
  return mutex;
}

std::shared_ptr<const AuthClient>& localClient()
{
  static std::shared_ptr<const AuthClient> client;
  return client;
}

}

void requireAuthTarget()
{
  static_cast<void>(resolveTarget());
}

void installLocalAuthClient(std::shared_ptr<const AuthClient> client)
{
  const std::scoped_lock lock(localMutex());
  localClient() = std::move(client);
}

std::shared_ptr<const AuthClient> filterAuthClient()
{
  {
    const std::scoped_lock lock(localMutex());
    if (localClient())
      return localClient();
  }
  static std::mutex mutex;
  static std::string cachedTarget;
  static std::string cachedCredential;
  static std::string cachedSecret;
  static std::shared_ptr<const AuthClient> client;

  const auto target = resolveTarget();
  const auto credential = ConfigService::getString("auth.credential");
  const auto secret = ConfigService::getString("auth.rpc_secret");
  const std::scoped_lock lock(mutex);
  if (!client || target != cachedTarget || credential != cachedCredential ||
      secret != cachedSecret) {
    cachedTarget = target;
    cachedCredential = credential;
    cachedSecret = secret;
    client = std::make_shared<AuthClient>(AuthClientConfig{
        .target = target, .credential = credential, .fleetSecret = secret});
  }
  return client;
}
