#pragma once

#include <push/push-outcome.hxx>
#include <shared/repositories/notification-token/notification-token-repository.hxx>

#include <drogon/utils/coroutine.h>

#include <optional>
#include <string>
#include <string_view>

struct PushOutcomeReport
{
  PushOutcome outcome{PushOutcome::Failed};
  std::string token;
};

std::optional<PushOutcomeReport> parsePushOutcome(std::string_view payload);

class PushOutcomeService
{
public:
  drogon::Task<bool> apply(std::string payload) const;

private:
  NotificationTokenRepository repository_;
};
