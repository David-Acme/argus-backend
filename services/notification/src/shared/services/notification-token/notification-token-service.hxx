#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/notification-token/notification-token-query.hxx>
#include <shared/repositories/notification-token/notification-token-repository.hxx>

class NotificationTokenService
{
public:
  NotificationTokenService() = default;

  drogon::Task<void>
  registerToken(const NotificationTokenCreateInput& input) const;

private:
  NotificationTokenRepository repository_;
};
