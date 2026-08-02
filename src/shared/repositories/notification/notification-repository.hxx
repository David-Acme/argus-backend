#pragma once
#include "notification-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/notification/notification-schema.hxx>

class NotificationRepository
{
public:
  NotificationRepository() = default;

  drogon::Task<NotificationSchema>
  create(const NotificationCreateInput& input) const;
  drogon::Task<std::vector<NotificationSchema>>
  createMany(const std::vector<NotificationCreateInput>& inputs) const;

  drogon::Task<std::vector<Json::Value>>
  findSync(const NotificationSyncFilter& filter) const;
  drogon::Task<std::optional<Json::Value>>
  findLastSync(const NotificationSyncFilter& filter) const;

  drogon::Task<void> markAsRead(int64_t userId,
                                const std::vector<int64_t>& ids) const;
};
