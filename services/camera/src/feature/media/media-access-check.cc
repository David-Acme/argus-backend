#include "media-access-check.hxx"

#include <auth/auth-access.hxx>
#include <auth/auth-client.hxx>
#include <auth/jwt-service.hxx>
#include <runtime/blocking-task.hxx>
#include <shared/utils/in-flight/in-flight.hxx>

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <memory>
#include <utility>
#include <vector>

MediaAccessCheck::MediaAccessCheck(Validate validate) : validate_(std::move(validate)) {}

MediaAccessCheck::Validate MediaAccessCheck::remote()
{
  auto verifier = std::make_shared<JwtService>(JwtRole::Verifier);
  return [verifier](const MediaCredential& credential) -> std::optional<UserRole> {
    if (verifier->verifyAccess(credential.token).empty())
      return std::nullopt;
    const auto client = filterAuthClient();
    if (!client)
      return credential.role;
    const auto verdict = client->validateToken({.accessToken = credential.token,
                                                .deviceHash = credential.deviceHash,
                                                .hasDeviceContext = true,
                                                .origin = credential.origin});
    if (!verdict)
      return credential.role;
    if (!verdict->valid() || !verdict->user().is_active())
      return std::nullopt;
    return userRoleFromString(verdict->user().role());
  };
}

MediaAccessVerdict MediaAccessCheck::judge(const MediaCredential& credential,
                                           const std::optional<UserRole>& current)
{
  if (!current)
    return MediaAccessVerdict::Expired;
  return *current == credential.role ? MediaAccessVerdict::Keep : MediaAccessVerdict::RoleChanged;
}

void MediaAccessCheck::add(const MediaAccessOpen& open)
{
  if (!open.connection || open.credential.token.empty())
    return;
  std::scoped_lock lock(mutex_);
  open_[open.connection.get()] = {.connection = open.connection, .credential = open.credential};
}

void MediaAccessCheck::remove(const drogon::WebSocketConnectionPtr& connection)
{
  std::scoped_lock lock(mutex_);
  open_.erase(connection.get());
}

drogon::Task<std::size_t> MediaAccessCheck::sweep()
{
  if (stopping_.load(std::memory_order_acquire) || sweeping_.exchange(true))
    co_return 0;
  const in_flight::Guard guard(inFlight_);
  std::vector<Entry> entries;
  {
    std::scoped_lock lock(mutex_);
    entries.reserve(open_.size());
    for (const auto& [key, entry] : open_)
      entries.push_back(entry);
  }
  std::vector<MediaAccessVerdict> verdicts;
  try {
    verdicts = co_await BlockingTask<std::vector<MediaAccessVerdict>>([this, entries]() {
      std::vector<MediaAccessVerdict> judged;
      judged.reserve(entries.size());
      for (const auto& entry : entries)
        judged.push_back(judge(entry.credential, validate_(entry.credential)));
      return judged;
    });
  }
  catch (const std::exception& error) {
    LOG_WARN << "Media access check failed: " << error.what();
    sweeping_.store(false);
    co_return 0;
  }
  std::size_t closed = 0;
  for (std::size_t i = 0; i < entries.size() && i < verdicts.size(); ++i) {
    if (verdicts[i] == MediaAccessVerdict::Keep)
      continue;
    const auto connection = entries[i].connection.lock();
    if (!connection || connection->disconnected())
      continue;
    connection->shutdown(drogon::CloseCode::kViolation,
                         verdicts[i] == MediaAccessVerdict::Expired ? "session_expired"
                                                                    : "role_changed");
    ++closed;
  }
  if (closed > 0)
    LOG_INFO << "Media access check: closed " << closed << " socket(s) whose access changed";
  sweeping_.store(false);
  co_return closed;
}

void MediaAccessCheck::start(double intervalSeconds)
{
  drogon::app().getLoop()->runEvery(intervalSeconds, [this]() {
    if (stopping_.load(std::memory_order_acquire))
      return;
    drogon::async_run([this]() -> drogon::Task<void> {
      co_await sweep();
    });
  });
}

void MediaAccessCheck::requestStop()
{
  stopping_.store(true, std::memory_order_release);
}

bool MediaAccessCheck::drained() const
{
  return inFlight_.load(std::memory_order_acquire) == 0;
}
