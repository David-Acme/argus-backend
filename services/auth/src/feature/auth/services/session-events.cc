#include "session-events.hxx"

#include <json/value.h>
#include <sync/auth-change-sink.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/sync-change.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <sync/user-action.hxx>

namespace
{
SocketEmitDto authContextFrame(const char* reason)
{
  SocketEmitDto frame;
  frame.operation = SyncOperation::AuthContextChanged;
  frame.option = TableName::User;
  frame.obj = Json::Value(Json::objectValue);
  frame.obj[session_events::kReasonField] = reason;
  frame.obj["resync"] = false;
  return frame;
}
}

std::string sessionRevocationScopeToString(SessionRevocationScope scope)
{
  switch (scope) {
    case SessionRevocationScope::Others:
      return "others";
    case SessionRevocationScope::All:
      return "all";
    case SessionRevocationScope::One:
      return "one";
  }
  return "one";
}

std::string sessionRevocationReasonToString(SessionRevocationReason reason)
{
  switch (reason) {
    case SessionRevocationReason::Logout:
      return "logout";
    case SessionRevocationReason::RefreshTokenReuse:
      return "refreshTokenReuse";
    case SessionRevocationReason::Revoked:
      return "revoked";
  }
  return "revoked";
}

drogon::Task<void> session_events::publishRevoked(const SessionRevokedEvent& event)
{
  const auto* sink = auth_change::getSink();
  if (sink == nullptr)
    co_return;

  SocketEmitDto frame = authContextFrame(kSessionRevoked);
  frame.obj["sessionId"] = event.sessionId;
  co_await sink->publishSessionChange(
      {.payload = sync_change::disconnectSessionPayload(
           frame, {.userId = event.userId, .sessionId = event.sessionId}),
       .client = event.client});

  Json::Value session(Json::objectValue);
  session["sessionId"] = event.sessionId;
  session["platform"] = sessionPlatformToString(event.platform);
  Json::Value revocation(Json::objectValue);
  revocation["event"] = kSessionRevoked;
  revocation["scope"] = sessionRevocationScopeToString(event.scope);
  revocation["reason"] = sessionRevocationReasonToString(event.reason);
  revocation["revokedBy"] = static_cast<Json::Int64>(event.actorId);
  co_await sink->publishAction({.event = {.userId = event.actorId,
                                          .recordId = event.userId,
                                          .tableName = TableName::User,
                                          .action = UserAction::Delete,
                                          .oldData = session,
                                          .newData = revocation,
                                          .ipAddress = ""},
                                .client = event.client});
}

drogon::Task<void>
session_events::publishChanged(const SessionsChangedEvent& event)
{
  const auto* sink = auth_change::getSink();
  if (sink == nullptr)
    co_return;
  co_await sink->publishSessionChange(
      {.payload = sync_change::userEmitPayload(authContextFrame(kSessionsChanged),
                                               {event.userId}),
       .client = event.client});
}
