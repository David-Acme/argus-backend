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
};
