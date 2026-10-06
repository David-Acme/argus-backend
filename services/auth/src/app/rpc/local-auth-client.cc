#include "local-auth-client.hxx"
#include "session-verdict-wire.hxx"

#include <auth/auth-access.hxx>
#include <condition_variable>
#include <drogon/drogon.h>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{

template <typename T>
struct Pending
{
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<T> value;
  bool done{false};
};

template <typename T>
std::optional<T> onLoop(std::function<drogon::Task<T>()> work,
                        std::chrono::milliseconds timeout)
{
  auto* loop = drogon::app().getLoop();
  if (loop == nullptr || loop->isInLoopThread()) {
    LOG_WARN << "Auth: an in-process verdict was asked for on the event loop";
    return std::nullopt;
  }
  auto pending = std::make_shared<Pending<T>>();
  loop->queueInLoop([pending, work = std::move(work)]() {
    drogon::async_run([pending, work]() -> drogon::Task<void> {
      std::optional<T> value;
      try {
        value = co_await work();
      }
      catch (const std::exception& error) {
        LOG_WARN << "Auth: in-process verdict failed: " << error.what();
      }
      {
        const std::scoped_lock lock(pending->mutex);
        pending->value = std::move(value);
        pending->done = true;
      }
      pending->ready.notify_all();
    });
  });
  std::unique_lock lock(pending->mutex);
  if (!pending->ready.wait_for(lock, timeout, [&pending] { return pending->done; }))
    return std::nullopt;
  return std::move(pending->value);
}

}

LocalAuthClient::LocalAuthClient(Dependencies dependencies)
    : dependencies_(dependencies)
{
}

std::optional<argus::auth::v1::ValidateTokenResponse>
LocalAuthClient::validateToken(const ValidateSessionInput& input) const
{
  const SessionValidationInput request{
      .accessToken = input.accessToken,
      .deviceHash = input.hasDeviceContext ? input.deviceHash : "",
      .hasDeviceContext = input.hasDeviceContext,
      .origin = input.origin.empty() ? SessionOrigin::Unknown
                                     : sessionOriginFromString(input.origin),
  };
  const SessionService* sessions = dependencies_.sessions;
  return onLoop<argus::auth::v1::ValidateTokenResponse>(
      [sessions, request]() -> drogon::Task<argus::auth::v1::ValidateTokenResponse> {
        const SessionVerdict verdict = co_await sessions->validate(request);
        argus::auth::v1::ValidateTokenResponse response;
        session_verdict_wire::write(verdict, response);
        co_return response;
      },
      dependencies_.timeout);
}

std::optional<bool>
LocalAuthClient::checkDeviceCredential(const std::string& secretHash) const
{
  const DeviceCredentialRepository* credentials = dependencies_.deviceCredentials;
  return onLoop<bool>(
      [credentials, secretHash]() -> drogon::Task<bool> {
        const auto active = co_await credentials->findActiveBySecretHash(secretHash);
        co_return active.has_value();
      },
      dependencies_.timeout);
}

void LocalAuthClient::install(Dependencies dependencies)
{
  installLocalAuthClient(std::make_shared<const LocalAuthClient>(dependencies));
}
