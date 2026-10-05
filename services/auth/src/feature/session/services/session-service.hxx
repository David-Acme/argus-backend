#pragma once

#include <auth/jwt-service.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/session/repositories/refresh-token/refresh-token-repository.hxx>
#include <feature/session/services/presence-signal.hxx>
#include <feature/session/services/session-context-cache.hxx>
#include <feature/session/services/session-revocation.hxx>
#include <optional>
#include <string>

class IdentityClient;

struct SessionValidationInput
{
  std::string accessToken;
  std::string deviceHash;
  bool hasDeviceContext{false};
  SessionOrigin origin{SessionOrigin::Unknown};
};

struct SessionVerdict
{
  bool valid{false};
  std::string reason;
  std::optional<UserContext> user;
  int64_t expiresAt{0};
  std::string sessionId;
};

class SessionService
{
public:
  struct Dependencies
  {
    JwtService jwtService;
    RefreshTokenRepository refreshTokenRepository;
    const IdentityClient* identity{nullptr};
  };

  struct Config
  {
    int64_t contextCacheSeconds{30};
  };

  static constexpr int64_t kLastSeenThrottleSeconds = 60;

  SessionService(Dependencies dependencies, Config config);

  [[nodiscard]] drogon::Task<SessionVerdict>
  validate(const SessionValidationInput& input) const;

  void forget(int64_t userId);

  [[nodiscard]] drogon::Task<bool> revokeUser(int64_t userId) const;

  void setPresenceSink(PresenceSignalSink* sink);

private:
  [[nodiscard]] drogon::Task<std::optional<RefreshTokenSchema>>
  sessionOf(int64_t userId, const std::string& accessToken) const;

  Dependencies dependencies_;
  SessionContextCache contextCache_;
  SessionRevocation revocation_;
  PresenceSignalSink* presenceSink_{nullptr};
  mutable PresenceSignalThrottle presenceThrottle_;
};
