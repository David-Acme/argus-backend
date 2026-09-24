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

  const auto session = co_await dependencies_.refreshTokenRepository
                           .findByAccessToken(userId, input.accessToken);
  if (!session)
    co_return SessionVerdict{};

  if (session->expiresAt <= std::time(nullptr))
    co_return rejected("Token expired");
  if (input.hasDeviceContext && session->deviceHash != input.deviceHash)
    co_return rejected("Device mismatch");

  co_return SessionVerdict{.valid = true,
                           .reason = "",
                           .user = context,
                           .expiresAt =
                               input.hasDeviceContext ? session->expiresAt : 0};
}

void SessionService::forget(int64_t userId)
{
  contextCache_.forget(userId);
}

drogon::Task<bool> SessionService::revokeUser(int64_t userId) const
{
  co_return co_await dependencies_.refreshTokenRepository.invalidateAllUser(
      userId);
}
