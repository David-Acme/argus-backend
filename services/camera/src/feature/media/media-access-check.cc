#include "media-access-check.hxx"

#include <auth/auth-access.hxx>
#include <auth/auth-client.hxx>
#include <auth/jwt-service.hxx>
#include <runtime/blocking-task.hxx>
#include <shared/utils/in-flight/in-flight.hxx>

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <charconv>
#include <memory>
#include <utility>
#include <vector>

namespace
{
std::optional<int64_t> subjectOf(const std::map<std::string, std::string>& claims)
{
  const auto sub = claims.find("sub");
  if (sub == claims.end())
    return std::nullopt;
  int64_t userId = 0;
  const auto [end, error] =
      std::from_chars(sub->second.data(), sub->second.data() + sub->second.size(), userId);
  if (error != std::errc() || end != sub->second.data() + sub->second.size() || userId <= 0)
    return std::nullopt;
  return userId;
}
}

MediaAccessCheck::MediaAccessCheck(Validate validate) : validate_(std::move(validate)) {}

MediaAccessCheck::Validate MediaAccessCheck::remote()
{
  auto verifier = std::make_shared<JwtService>(JwtRole::Verifier);
  return [verifier](const MediaCredential& credential) -> std::optional<MediaIdentity> {
    const auto subject = subjectOf(verifier->verifyAccess(credential.token));
    if (!subject)
      return std::nullopt;
    const auto client = filterAuthClient();
    if (!client)
      return MediaIdentity{.userId = *subject, .role = credential.role};
    const auto verdict = client->validateToken({.accessToken = credential.token,
                                                .deviceHash = credential.deviceHash,
                                                .hasDeviceContext = true,
                                                .origin = credential.origin});
    if (!verdict)
      return MediaIdentity{.userId = *subject, .role = credential.role};
    if (!verdict->valid() || !verdict->user().is_active())
      return std::nullopt;
    return MediaIdentity{.userId = verdict->user().user_id(),
                         .role = userRoleFromString(verdict->user().role())};
  };
}

MediaAccessVerdict MediaAccessCheck::judge(const MediaCredential& credential,
                                           const std::optional<MediaIdentity>& current)
{
  if (!current || current->userId != credential.userId)
    return MediaAccessVerdict::Expired;
  return current->role == credential.role ? MediaAccessVerdict::Keep
                                          : MediaAccessVerdict::RoleChanged;
}

const char* MediaAccessCheck::closeReason(MediaAccessVerdict verdict)
{
  return verdict == MediaAccessVerdict::RoleChanged ? "role_changed" : "session_expired";
}

void MediaAccessCheck::close(const drogon::WebSocketConnectionPtr& connection,
                             MediaAccessVerdict verdict)
{
  if (!connection || connection->disconnected())
    return;
  connection->shutdown(drogon::CloseCode::kViolation, closeReason(verdict));
}

void MediaAccessCheck::add(const MediaAccessOpen& open)
{
  if (!open.connection || open.credential.token.empty())
    return;
  std::scoped_lock lock(mutex_);
  open_[open.connection.get()] = {.connection = open.connection,
                                  .credential = open.credential,
                                  .renewedAt = std::nullopt};
}

void MediaAccessCheck::remove(const drogon::WebSocketConnectionPtr& connection)
{
  std::scoped_lock lock(mutex_);
  open_.erase(connection.get());
}

std::size_t MediaAccessCheck::sweepNow()
{
  std::vector<Entry> entries;
  {
    std::scoped_lock lock(mutex_);
    entries.reserve(open_.size());
    for (const auto& [key, entry] : open_)
      entries.push_back(entry);
  }
  std::size_t closed = 0;
  for (const auto& entry : entries) {
    const MediaAccessVerdict verdict = judge(entry.credential, validate_(entry.credential));
    if (verdict == MediaAccessVerdict::Keep)
      continue;
    const auto connection = entry.connection.lock();
    if (!connection || connection->disconnected())
      continue;
    {
      std::scoped_lock lock(mutex_);
      const auto current = open_.find(connection.get());
      if (current == open_.end() || current->second.credential.token != entry.credential.token)
        continue;
    }
    close(connection, verdict);
    ++closed;
  }
  if (closed > 0)
    LOG_INFO << "Media access check: closed " << closed << " socket(s) whose access changed";
  return closed;
}

drogon::Task<std::size_t> MediaAccessCheck::sweep()
{
  if (stopping_.load(std::memory_order_acquire) || sweeping_.exchange(true))
    co_return 0;
  const in_flight::Guard guard(inFlight_);
  std::size_t closed = 0;
  try {
    closed = co_await BlockingTask<std::size_t>([this]() { return sweepNow(); });
  }
  catch (const std::exception& error) {
    LOG_WARN << "Media access check failed: " << error.what();
  }
  sweeping_.store(false);
  co_return closed;
}

MediaRenewal MediaAccessCheck::renewNow(const MediaRenewInput& input)
{
  if (!input.connection)
    return MediaRenewal::Unknown;
  MediaCredential candidate;
  {
    std::scoped_lock lock(mutex_);
    const auto entry = open_.find(input.connection.get());
    if (entry == open_.end())
      return MediaRenewal::Unknown;
    if (entry->second.renewedAt && input.at - *entry->second.renewedAt < kMinRenewInterval)
      return MediaRenewal::Throttled;
    entry->second.renewedAt = input.at;
    candidate = entry->second.credential;
  }
  candidate.token = input.token;
  const MediaAccessVerdict verdict =
      input.token.empty() ? MediaAccessVerdict::Expired : judge(candidate, validate_(candidate));
  if (verdict != MediaAccessVerdict::Keep) {
    close(input.connection, verdict);
    return MediaRenewal::Closed;
  }
  std::scoped_lock lock(mutex_);
  const auto entry = open_.find(input.connection.get());
  if (entry == open_.end())
    return MediaRenewal::Unknown;
  entry->second.credential.token = std::move(candidate.token);
  return MediaRenewal::Renewed;
}

drogon::Task<MediaRenewal> MediaAccessCheck::renew(MediaRenewInput input)
{
  if (stopping_.load(std::memory_order_acquire))
    co_return MediaRenewal::Unknown;
  const in_flight::Guard guard(inFlight_);
  co_return co_await BlockingTask<MediaRenewal>(
      [this, input = std::move(input)]() { return renewNow(input); });
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
