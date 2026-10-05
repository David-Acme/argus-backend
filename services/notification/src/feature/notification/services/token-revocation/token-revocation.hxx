#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/notification/repositories/notification-token/notification-token-repository.hxx>
#include <json/value.h>
#include <nats/nats-bus.hxx>
#include <shared/services/task-gate/task-gate.hxx>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct TokenRevocation
{
  int64_t userId{0};
  std::string sessionId;
};

class TokenRevocationService
{
public:
  TokenRevocationService() = default;

  drogon::Task<int64_t> revoke(const TokenRevocation& revocation) const;

private:
  NotificationTokenRepository repository_;
};

namespace token_revocation
{
inline constexpr const char* kSessionDurable = "notification-auth-session";
inline constexpr const char* kIdentityDurable = "notification-identity-user";

std::optional<TokenRevocation> fromSessionChange(const Json::Value& change);

std::optional<TokenRevocation> fromIdentityChange(const Json::Value& change);

struct FeedInput
{
  std::shared_ptr<NatsBus> bus;
  std::shared_ptr<const TokenRevocationService> service;
  std::shared_ptr<TaskGate> tasks;
};

void subscribe(const FeedInput& input);
}
