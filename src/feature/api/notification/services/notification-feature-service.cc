#include "notification-feature-service.hxx"

drogon::Task<void>
NotificationFeatureService::markAsRead(int64_t userId,
                                       const std::vector<int64_t>& ids) const
{
  co_await notificationService_.markAsRead(userId, ids);
  co_return;
}
