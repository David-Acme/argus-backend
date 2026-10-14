#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/notification-token/notification-token-query.hxx>
#include <feature/notification/services/notification-token/notification-token-service.hxx>

class NotificationTokenFeatureService
{
public:
  NotificationTokenFeatureService() = default;

  drogon::Task<void>
  registerToken(const NotificationTokenCreateInput& input) const;

  drogon::Task<bool>
  unregisterToken(const NotificationTokenRemoveInput& input) const;

private:
  NotificationTokenService notificationTokenService_;
};
