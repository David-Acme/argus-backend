#include "notification-token-service.hxx"

drogon::Task<void> NotificationTokenService::registerToken(
    const NotificationTokenCreateInput& input) const
{
  co_await repository_.upsert(input);
  co_return;
}

drogon::Task<void>
NotificationTokenService::removeDevice(int64_t userId,
                                       const std::string& deviceHash) const
{
  co_await repository_.deleteByDevice(userId, deviceHash);
  co_return;
}
