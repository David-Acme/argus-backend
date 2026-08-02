#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/notification-token/notification-token-query.hxx>
#include <shared/repositories/notification-token/notification-token-repository.hxx>
#include <string>

class NotificationTokenService
{
public:
  NotificationTokenService() = default;

  drogon::Task<void>
  registerToken(const NotificationTokenCreateInput& input) const;
  drogon::Task<void> removeDevice(int64_t userId,
                                  const std::string& deviceHash) const;

private:
  NotificationTokenRepository repository_;
};
