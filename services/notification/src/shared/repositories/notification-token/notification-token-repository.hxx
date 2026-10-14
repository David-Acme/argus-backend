#pragma once
#include "notification-token-query.hxx"

#include <drogon/utils/coroutine.h>
#include <feature/notification/schemas/notification-token/notification-token-schema.hxx>

class NotificationTokenRepository
{
public:
  NotificationTokenRepository() = default;

  drogon::Task<void> upsert(const NotificationTokenCreateInput& input) const;
  drogon::Task<std::vector<NotificationTokenSchema>>
  findByUser(int64_t userId) const;
  drogon::Task<int64_t>
  removeForSession(const NotificationTokenSessionInput& input) const;
  drogon::Task<int64_t> removeForUser(int64_t userId) const;
};
