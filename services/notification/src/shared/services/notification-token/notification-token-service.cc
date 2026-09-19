#include "notification-token-service.hxx"

drogon::Task<void> NotificationTokenService::registerToken(
    const NotificationTokenCreateInput& input) const
{
  co_await repository_.upsert(input);
  co_return;
}
