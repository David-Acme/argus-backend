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
  drogon::Task<int64_t>
  removeForSession(const NotificationTokenSessionInput& input) const;
  drogon::Task<int64_t> removeForUser(int64_t userId) const;
  drogon::Task<std::vector<NotificationTokenSchema>>
  findPushByUser(int64_t userId) const;
  drogon::Task<int64_t>
  removeOwnToken(const NotificationTokenRemoveInput& input) const;
  drogon::Task<int64_t> removeByToken(std::string token) const;
};
