#pragma once

#include <feature/memory/services/memory/reminder-call-scheduler.hxx>
#include <notification/notification-client.hxx>

#include <memory>

class NotificationReminderCalls final : public ReminderCallScheduler
{
public:
  explicit NotificationReminderCalls(std::shared_ptr<const NotificationClient> client);

  [[nodiscard]] bool schedule(const ReminderCallRequest& request) const override;

private:
  std::shared_ptr<const NotificationClient> client_;
};
