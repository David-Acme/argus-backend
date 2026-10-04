#include "session-revocation.hxx"

#include <auth/auth-errors.hxx>
#include <ctime>
#include <errors/response-exception.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <trantor/utils/Logger.h>

namespace
{
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
}

drogon::Task<std::vector<std::string>>
SessionRevocation::revoke(const SessionRevocationInput& input) const
{
  if (input.scope != SessionRevocationScope::All && input.sessionId.empty())
    co_return std::vector<std::string>{};

  std::vector<std::string> revoked;
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    const auto active = co_await repository_.listActive(
        {.userId = input.userId,
         .now = static_cast<int64_t>(std::time(nullptr)),
         .client = transaction.get()});

    const SessionLookupInput lookup{.userId = input.userId,
                                    .sessionId = input.sessionId,
                                    .client = transaction.get()};
    switch (input.scope) {
      case SessionRevocationScope::One:
        static_cast<void>(co_await repository_.invalidateSession(lookup));
        break;
      case SessionRevocationScope::Others:
        static_cast<void>(co_await repository_.invalidateOtherSessions(lookup));
        break;
      case SessionRevocationScope::All:
        static_cast<void>(co_await repository_.invalidateAllUser(
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
