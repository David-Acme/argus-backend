#include "session-management-service.hxx"

#include <algorithm>
#include <array>
#include <auth/auth-errors.hxx>
#include <ctime>
#include <errors/response-exception.hxx>
#include <openssl/rand.h>
#include <string_view>
#include <utility>

namespace
{
constexpr std::size_t kSessionIdBytes = 16;

bool isLowerHex(char value)
{
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

int64_t nowSeconds()
{
  return static_cast<int64_t>(std::time(nullptr));
}

SessionView viewOf(const RefreshTokenSchema& row,
                   const std::string& currentSessionId)
{
  return {.id = row.sessionId,
          .platform = row.platform,
          .deviceName = row.deviceName,
          .createdAt =
              row.sessionCreatedAt > 0 ? row.sessionCreatedAt : row.createdAt,
          .lastSeenAt = row.lastSeenAt > 0 ? row.lastSeenAt : row.createdAt,
          .expiresAt = row.expiresAt,
          .current = row.sessionId == currentSessionId};
}

ResponseRevokeSessionsDto revocationResult(std::vector<std::string> revoked,
                                           const SessionOwnerInput& owner)
{
  const bool current = std::ranges::find(revoked, owner.currentSessionId) !=
                       revoked.end();
  return ResponseRevokeSessionsDto{.revoked = std::move(revoked),
                                   .current = current};
}

SessionRevocationReason reasonFor(const UserSessionsInput& input)
{
  return input.actor.userId == input.userId
             ? SessionRevocationReason::Revoked
             : SessionRevocationReason::RevokedByOwner;
}

SessionOwnerInput callerAgainst(const UserSessionsInput& input)
{
  return {.userId = input.userId,
          .currentSessionId = input.actor.userId == input.userId
                                  ? input.actor.currentSessionId
                                  : std::string{}};
}
}

SessionManagementService::SessionManagementService(Dependencies dependencies)
    : dependencies_(dependencies)
{
}

bool SessionManagementService::isSessionId(const std::string& value)
{
  return value.size() == kSessionIdBytes * 2 &&
         std::ranges::all_of(value, isLowerHex);
}

std::string SessionManagementService::newSessionId()
{
  std::array<unsigned char, kSessionIdBytes> buffer{};
  if (RAND_bytes(buffer.data(), static_cast<int>(buffer.size())) != 1)
    throw ResponseException(AuthErrors::TokenIssuanceFailed);

  constexpr std::string_view kHexDigits = "0123456789abcdef";
  std::string id;
  id.reserve(buffer.size() * 2);
  for (const unsigned char byte : buffer) {
    id.push_back(kHexDigits[byte >> 4U]);
    id.push_back(kHexDigits[byte & 0x0FU]);
  }
  return id;
}

drogon::Task<ResponseListSessionsDto>
SessionManagementService::list(const SessionOwnerInput& input) const
{
  const auto rows = co_await dependencies_.refreshTokenRepository.listActive(
      {.userId = input.userId, .now = nowSeconds(), .client = nullptr});

  ResponseListSessionsDto result;
  result.sessions.reserve(rows.size());
  for (const auto& row : rows)
    result.sessions.push_back(viewOf(row, input.currentSessionId));
  std::ranges::stable_partition(result.sessions, &SessionView::current);
  co_return result;
}

drogon::Task<ResponseUserSessionsDto>
SessionManagementService::listEveryUser(const SessionOwnerInput& actor) const
{
  const auto rows =
      co_await dependencies_.refreshTokenRepository.listAllActive(nowSeconds());

  ResponseUserSessionsDto result;
  for (const auto& row : rows) {
    if (result.users.empty() || result.users.back().userId != row.userId)
      result.users.push_back({.userId = row.userId, .sessions = {}});
    result.users.back().sessions.push_back(viewOf(
        row,
        row.userId == actor.userId ? actor.currentSessionId : std::string{}));
  }
  const auto latest = [](const UserSessionsView& user) {
    return user.sessions.front().lastSeenAt;
  };
  std::ranges::stable_sort(result.users, std::ranges::greater{}, latest);
  co_return result;
}

drogon::Task<ResponseListSessionsDto>
SessionManagementService::listOfUser(const UserSessionsInput& input) const
{
  co_return co_await list(callerAgainst(input));
}

drogon::Task<ResponseRevokeSessionsDto>
SessionManagementService::revokeOne(const RevokeSessionInput& input) const
{
  if (!isSessionId(input.sessionId))
    throw ResponseException(AuthErrors::SessionNotFound);

  auto revoked = co_await revoke({.userId = input.owner.userId,
                                  .actorId = input.owner.userId,
                                  .scope = SessionRevocationScope::One,
                                  .sessionId = input.sessionId,
                                  .reason = SessionRevocationReason::Revoked});
  if (revoked.empty())
    throw ResponseException(AuthErrors::SessionNotFound);
  co_return revocationResult(std::move(revoked), input.owner);
}

drogon::Task<ResponseRevokeSessionsDto>
SessionManagementService::revokeScope(const RevokeSessionScopeInput& input) const
{
  auto revoked =
      co_await revoke({.userId = input.owner.userId,
                       .actorId = input.owner.userId,
                       .scope = input.scope,
                       .sessionId = input.owner.currentSessionId,
                       .reason = SessionRevocationReason::Revoked});
  co_return revocationResult(std::move(revoked), input.owner);
}

drogon::Task<ResponseRevokeSessionsDto>
SessionManagementService::revokeUserSession(
    const RevokeUserSessionInput& input) const
{
  if (input.userId <= 0 || !isSessionId(input.sessionId))
    throw ResponseException(AuthErrors::SessionNotFound);

  const UserSessionsInput target{.actor = input.actor, .userId = input.userId};
  auto revoked = co_await revoke({.userId = input.userId,
                                  .actorId = input.actor.userId,
                                  .scope = SessionRevocationScope::One,
                                  .sessionId = input.sessionId,
                                  .reason = reasonFor(target)});
  if (revoked.empty())
    throw ResponseException(AuthErrors::SessionNotFound);
  co_return revocationResult(std::move(revoked), callerAgainst(target));
}

drogon::Task<ResponseRevokeSessionsDto>
SessionManagementService::revokeUserSessions(
    const UserSessionsInput& input) const
{
  if (input.userId <= 0)
    throw ResponseException(AuthErrors::UserNotFound);

  auto revoked = co_await revoke({.userId = input.userId,
                                  .actorId = input.actor.userId,
                                  .scope = SessionRevocationScope::All,
                                  .sessionId = "",
                                  .reason = reasonFor(input)});
  co_return revocationResult(std::move(revoked), callerAgainst(input));
}

drogon::Task<std::vector<std::string>>
SessionManagementService::revoke(const SessionRevocationInput& input) const
{
  co_return co_await revocation_.revoke(input);
}
