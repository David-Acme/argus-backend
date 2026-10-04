#include "session-management-service.hxx"

#include <algorithm>
#include <array>
#include <auth/auth-errors.hxx>
#include <ctime>
#include <errors/response-exception.hxx>
#include <openssl/rand.h>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <string_view>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
constexpr std::size_t kSessionIdBytes = 16;

bool isLowerHex(char value)
{
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

bool targets(const SessionRevocationInput& input,
             const RefreshTokenSchema& session)
{
  switch (input.scope) {
    case SessionRevocationScope::One:
      return session.sessionId == input.sessionId;
    case SessionRevocationScope::Others:
      return session.sessionId != input.sessionId;
    case SessionRevocationScope::All:
      return true;
  }
  return false;
}

ResponseRevokeSessionsDto revocationResult(std::vector<std::string> revoked,
                                           const SessionOwnerInput& owner)
{
  const bool current = std::ranges::find(revoked, owner.currentSessionId) !=
                       revoked.end();
  return ResponseRevokeSessionsDto{.revoked = std::move(revoked),
                                   .current = current};
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
      {.userId = input.userId,
       .now = static_cast<int64_t>(std::time(nullptr)),
       .client = nullptr});

  ResponseListSessionsDto result;
  result.sessions.reserve(rows.size());
  for (const auto& row : rows)
    result.sessions.push_back(
        {.id = row.sessionId,
         .platform = row.platform,
         .deviceName = row.deviceName,
         .createdAt = row.sessionCreatedAt > 0 ? row.sessionCreatedAt
                                               : row.createdAt,
         .lastSeenAt = row.lastSeenAt > 0 ? row.lastSeenAt : row.createdAt,
         .expiresAt = row.expiresAt,
         .current = row.sessionId == input.currentSessionId});
  std::ranges::stable_partition(result.sessions, &SessionView::current);
  co_return result;
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

drogon::Task<std::vector<std::string>>
SessionManagementService::revoke(const SessionRevocationInput& input) const
{
  if (input.scope != SessionRevocationScope::All && input.sessionId.empty())
    co_return std::vector<std::string>{};

  const auto& repository = dependencies_.refreshTokenRepository;
  std::vector<std::string> revoked;
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    const auto active = co_await repository.listActive(
        {.userId = input.userId,
         .now = static_cast<int64_t>(std::time(nullptr)),
         .client = transaction.get()});

    const SessionLookupInput lookup{.userId = input.userId,
                                    .sessionId = input.sessionId,
                                    .client = transaction.get()};
    switch (input.scope) {
      case SessionRevocationScope::One:
        static_cast<void>(co_await repository.invalidateSession(lookup));
        break;
      case SessionRevocationScope::Others:
        static_cast<void>(co_await repository.invalidateOtherSessions(lookup));
        break;
      case SessionRevocationScope::All:
        static_cast<void>(co_await repository.invalidateAllUser(
            input.userId, transaction.get()));
        break;
    }

    for (const auto& session : active) {
      if (!targets(input, session))
        continue;
      revoked.push_back(session.sessionId);
      co_await session_events::publishRevoked(
          {.userId = input.userId,
           .actorId = input.actorId,
           .sessionId = session.sessionId,
           .platform = session.platform,
           .scope = input.scope,
           .reason = input.reason,
           .client = transaction.get()});
    }
    if (!revoked.empty())
      co_await session_events::publishChanged(
          {.userId = input.userId, .client = transaction.get()});

    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(AuthErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }

  if (!revoked.empty())
    LOG_INFO << "Auth: revoked " << revoked.size() << " session(s) of user "
             << input.userId << " ("
             << sessionRevocationReasonToString(input.reason) << ", "
             << sessionRevocationScopeToString(input.scope) << ")";
  co_return revoked;
}
