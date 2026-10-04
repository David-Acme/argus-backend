#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/session/repositories/refresh-token/refresh-token-repository.hxx>
#include <feature/session/services/session-events.hxx>
#include <string>
#include <vector>

struct SessionRevocationInput
{
  int64_t userId{0};
  int64_t actorId{0};
  SessionRevocationScope scope{SessionRevocationScope::One};
  std::string sessionId;
  SessionRevocationReason reason{SessionRevocationReason::Revoked};
};

class SessionRevocation
{
public:
  [[nodiscard]] drogon::Task<std::vector<std::string>>
  revoke(const SessionRevocationInput& input) const;

private:
  RefreshTokenRepository repository_;
};
