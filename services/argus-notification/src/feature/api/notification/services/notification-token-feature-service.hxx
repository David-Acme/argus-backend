#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/notification-token/notification-token-query.hxx>
#include <shared/services/notification-token/notification-token-service.hxx>
#include <string>

class NotificationTokenFeatureService
{
public:
  NotificationTokenFeatureService() = default;

  drogon::Task<void>
  registerToken(const NotificationTokenCreateInput& input) const;
  drogon::Task<void> removeDevice(int64_t userId,
                                  const std::string& deviceHash) const;

private:
  NotificationTokenService notificationTokenService_;
};
