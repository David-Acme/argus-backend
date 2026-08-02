#include "notification-token-feature-service.hxx"

drogon::Task<void> NotificationTokenFeatureService::registerToken(
    const NotificationTokenCreateInput& input) const
{
  co_await notificationTokenService_.registerToken(input);
  co_return;
}

drogon::Task<void> NotificationTokenFeatureService::removeDevice(
    int64_t userId, const std::string& deviceHash) const
{
  co_await notificationTokenService_.removeDevice(userId, deviceHash);
  co_return;
}
