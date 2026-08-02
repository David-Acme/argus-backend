#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <vector>

class NotificationService
{
public:
  NotificationService() = default;

  drogon::Task<void>
  createAndEmit(int64_t userId, const NotificationCreateInput& input) const;
  drogon::Task<void>
  createAndEmitMany(const std::vector<int64_t>& userIds,
                    const NotificationCreateInput& input) const;
  drogon::Task<void> markAsRead(int64_t userId,
                                const std::vector<int64_t>& ids) const;

private:
  NotificationRepository repository_;
  SocketService socketService_;
};
