#pragma once

#include <feature/call/services/call-ports.hxx>
#include <shared/services/notification/notification-service.hxx>

class NotificationCallSink final : public CallNotificationSink
{
public:
  explicit NotificationCallSink(std::shared_ptr<const NotificationService> notifications);

  drogon::Task<bool> notify(const CallNotice& notice) const override;

private:
  std::shared_ptr<const NotificationService> service_;
};
