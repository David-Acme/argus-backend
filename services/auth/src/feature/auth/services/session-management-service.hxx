#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/auth/dtos/response-list-sessions-dto.hxx>
#include <feature/auth/dtos/response-revoke-sessions-dto.hxx>
#include <feature/auth/dtos/response-user-sessions-dto.hxx>
#include <feature/session/repositories/refresh-token/refresh-token-repository.hxx>
#include <feature/session/services/session-revocation.hxx>
#include <string>
#include <vector>

struct SessionOwnerInput
{
  int64_t userId{0};
  std::string currentSessionId;
  UserRole role{UserRole::Unknown};
};

struct RevokeSessionInput
{
  SessionOwnerInput owner;
  std::string sessionId;
};

struct RevokeSessionScopeInput
{
  SessionOwnerInput owner;
  SessionRevocationScope scope{SessionRevocationScope::Others};
};

struct UserSessionsInput
{
  SessionOwnerInput actor;
  int64_t userId{0};
};

struct RevokeUserSessionInput
{
  SessionOwnerInput actor;
  int64_t userId{0};
  std::string sessionId;
};

class SessionManagementService
{
public:
  struct Dependencies
  {
    RefreshTokenRepository refreshTokenRepository;
  };

  explicit SessionManagementService(Dependencies dependencies);

  [[nodiscard]] drogon::Task<ResponseListSessionsDto>
  list(const SessionOwnerInput& input) const;

  [[nodiscard]] drogon::Task<ResponseRevokeSessionsDto>
  revokeOne(const RevokeSessionInput& input) const;

  [[nodiscard]] drogon::Task<ResponseRevokeSessionsDto>
  revokeScope(const RevokeSessionScopeInput& input) const;

  [[nodiscard]] drogon::Task<ResponseUserSessionsDto>
  listEveryUser(const SessionOwnerInput& actor) const;

  [[nodiscard]] drogon::Task<ResponseListSessionsDto>
  listOfUser(const UserSessionsInput& input) const;

  [[nodiscard]] drogon::Task<ResponseRevokeSessionsDto>
  revokeUserSession(const RevokeUserSessionInput& input) const;

  [[nodiscard]] drogon::Task<ResponseRevokeSessionsDto>
  revokeUserSessions(const UserSessionsInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<std::string>>
  revoke(const SessionRevocationInput& input) const;

  [[nodiscard]] static bool isSessionId(const std::string& value);

  [[nodiscard]] static std::string newSessionId();

private:
  Dependencies dependencies_;
  SessionRevocation revocation_;
};
