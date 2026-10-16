#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <shared/services/notification/notification-service.hxx>
#include <vector>

class NotificationFeatureService
{
public:
  explicit NotificationFeatureService(std::shared_ptr<const NotificationService> notifications);

  drogon::Task<void> markAsRead(int64_t userId,
                                const std::vector<int64_t>& ids) const;

  [[nodiscard]] drogon::Task<bool> remove(const NotificationRef& ref) const;

  [[nodiscard]] drogon::Task<bool> removeThread(
      const NotificationThreadRef& ref) const;

  drogon::Task<int64_t> ackDeliveries(
      int64_t userId, const std::vector<int64_t>& notificationIds) const;

  drogon::Task<Json::Value> deliverySummary(int64_t since) const;

private:
  std::shared_ptr<const NotificationService> notificationService_;
};
