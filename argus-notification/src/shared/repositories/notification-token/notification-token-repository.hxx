#pragma once
#include "notification-token-query.hxx"

#include <drogon/utils/coroutine.h>
#include <shared/schemas/notification-token/notification-token-schema.hxx>

class NotificationTokenRepository
{
public:
  NotificationTokenRepository() = default;

  drogon::Task<void> upsert(const NotificationTokenCreateInput& input) const;
  drogon::Task<std::vector<NotificationTokenSchema>>
  findByUser(int64_t userId) const;
  drogon::Task<void> deleteByDevice(int64_t userId,
                                    const std::string& deviceHash) const;
  drogon::Task<void> removeAllByUser(int64_t userId) const;
};
