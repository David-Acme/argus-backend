#include "session-service.hxx"

#include <charconv>
#include <ctime>
#include <map>
#include <string>
#include <utility>

namespace
{
SessionVerdict rejected(std::string reason)
{
  SessionVerdict verdict;
  verdict.reason = std::move(reason);
  return verdict;
}

int64_t subOf(const std::map<std::string, std::string>& claims)
{
  const auto sub = claims.find("sub");
  if (sub == claims.end())
    return 0;
  const std::string& text = sub->second;
  int64_t userId = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), userId);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    return 0;
  return userId;
}
}

SessionService::SessionService(Dependencies dependencies, Config config)
    : dependencies_(std::move(dependencies)),
      contextCache_(dependencies_.identity,
                    SessionContextCache::Config{
                        .ttlSeconds = config.contextCacheSeconds})
{
}

drogon::Task<SessionVerdict>
SessionService::validate(const SessionValidationInput& input) const
{
  const auto claims = dependencies_.jwtService.verifyAccess(input.accessToken);
  if (claims.empty())
    co_return SessionVerdict{};

  const int64_t userId = subOf(claims);
  if (userId <= 0)
    co_return SessionVerdict{};

  const auto context = co_await contextCache_.resolve(userId);
  if (!context)
    co_return SessionVerdict{};
  if (!context->isActive)
    co_return rejected("User account is disabled");

  const auto session = co_await sessionOf(userId, input.accessToken);
  if (!session)
    co_return SessionVerdict{};

  const auto now = static_cast<int64_t>(std::time(nullptr));
  if (session->expiresAt <= now)
    co_return rejected("Token expired");
  if (input.hasDeviceContext && session->deviceHash != input.deviceHash)
    co_return rejected("Device mismatch");

  if (now - session->lastSeenAt >= kLastSeenThrottleSeconds)
    static_cast<void>(co_await dependencies_.refreshTokenRepository.touch(
        {.rowId = session->id,
         .now = now,
         .throttleSeconds = kLastSeenThrottleSeconds}));

  co_return SessionVerdict{.valid = true,
                           .reason = "",
                           .user = context,
                           .expiresAt =
                               input.hasDeviceContext ? session->expiresAt : 0,
                           .sessionId = session->sessionId};
}

drogon::Task<std::optional<RefreshTokenSchema>>
SessionService::sessionOf(int64_t userId, const std::string& accessToken) const
{
  const auto& repository = dependencies_.refreshTokenRepository;
  auto session = co_await repository.findByAccessToken(userId, accessToken);
  if (!session || !session->sessionId.empty())
    co_return session;
  co_await repository.adoptLegacySessions(userId);
  co_return co_await repository.findByAccessToken(userId, accessToken);
}

void SessionService::forget(int64_t userId)
{
  contextCache_.forget(userId);
}

drogon::Task<bool> SessionService::revokeUser(int64_t userId) const
{
  co_await dependencies_.refreshTokenRepository.adoptLegacySessions(userId);
  const auto revoked = co_await revocation_.revoke(
      {.userId = userId,
       .actorId = 0,
       .scope = SessionRevocationScope::All,
       .sessionId = "",
       .reason = SessionRevocationReason::AccountDisabled});
  co_return !revoked.empty();
}
