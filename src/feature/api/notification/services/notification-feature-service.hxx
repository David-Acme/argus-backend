#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <shared/services/notification/notification-service.hxx>
#include <vector>

class NotificationFeatureService
{
public:
  NotificationFeatureService() = default;

  drogon::Task<void> markAsRead(int64_t userId,
                                const std::vector<int64_t>& ids) const;

private:
  NotificationService notificationService_;
};
