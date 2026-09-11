#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/contracts/user-change-sink.hxx>
#include <vector>

class NotificationService
{
public:
  NotificationService() = default;

  drogon::Task<std::vector<NotificationSchema>>
  createManyAndEmit(const std::vector<int64_t>& userIds,
                    const NotificationCreateInput& input) const;

  drogon::Task<void> markAsRead(int64_t userId,
                                const std::vector<int64_t>& ids) const;

private:
  NotificationRepository repository_;
};
