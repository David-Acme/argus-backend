#include "notification-token-feature-service.hxx"

drogon::Task<void> NotificationTokenFeatureService::registerToken(
    const NotificationTokenCreateInput& input) const
{
  co_await notificationTokenService_.registerToken(input);
  co_return;
}
