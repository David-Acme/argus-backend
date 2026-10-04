#pragma once

#include <feature/call/services/call-ports.hxx>
#include <shared/services/notification/notification-service.hxx>

class NotificationCallSink final : public CallNotificationSink
{
public:
  explicit NotificationCallSink(NotificationService::Dependencies dependencies);

  drogon::Task<bool> notify(const CallNotice& notice) const override;

private:
  NotificationService service_;
};
