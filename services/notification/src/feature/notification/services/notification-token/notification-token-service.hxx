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

  drogon::Task<bool>
  unregisterToken(const NotificationTokenRemoveInput& input) const;

private:
  NotificationTokenRepository repository_;
};
