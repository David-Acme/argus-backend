#pragma once

#include <auth/session-platform.hxx>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <string>

enum class SessionRevocationScope : std::uint8_t
{
  One,
  Others,
  All
};

enum class SessionRevocationReason : std::uint8_t
{
  Logout,
  Revoked,
  RefreshTokenReuse,
  RevokedByOwner,
  AccountDisabled
};

[[nodiscard]] std::string
sessionRevocationScopeToString(SessionRevocationScope scope);

[[nodiscard]] std::string
sessionRevocationReasonToString(SessionRevocationReason reason);

struct SessionRevokedEvent
{
  int64_t userId{0};
  int64_t actorId{0};
  std::string sessionId;
  SessionPlatform platform{SessionPlatform::Unknown};
  SessionRevocationScope scope{SessionRevocationScope::One};
  SessionRevocationReason reason{SessionRevocationReason::Revoked};
  drogon::orm::DbClient* client{nullptr};
};

struct SessionsChangedEvent
{
  int64_t userId{0};
  drogon::orm::DbClient* client{nullptr};
};

namespace session_events
{
inline constexpr const char* kReasonField = "reason";
inline constexpr const char* kCauseField = "cause";
inline constexpr const char* kUserIdField = "userId";
inline constexpr const char* kSessionRevoked = "sessionRevoked";
inline constexpr const char* kSessionsChanged = "sessionsChanged";
inline constexpr const char* kUserSessionsChanged = "userSessionsChanged";

[[nodiscard]] drogon::Task<void> publishRevoked(const SessionRevokedEvent& event);

[[nodiscard]] drogon::Task<void>
publishChanged(const SessionsChangedEvent& event);
}
