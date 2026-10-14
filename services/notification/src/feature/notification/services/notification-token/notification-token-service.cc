#include "notification-token-service.hxx"

drogon::Task<void> NotificationTokenService::registerToken(
    const NotificationTokenCreateInput& input) const
{
  co_await repository_.upsert(input);
  co_return;
}

drogon::Task<bool> NotificationTokenService::unregisterToken(
    const NotificationTokenRemoveInput& input) const
{
  co_return co_await repository_.removeOwnToken(input) > 0;
}
