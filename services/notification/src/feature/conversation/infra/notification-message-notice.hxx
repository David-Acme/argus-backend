#pragma once

#include <feature/conversation/services/conversation-ports.hxx>
#include <shared/services/notification/notification-service.hxx>

#include <memory>

class NotificationMessageNotice final : public MessageNoticeWriter
{
public:
  explicit NotificationMessageNotice(std::shared_ptr<const NotificationService> notifications);

  [[nodiscard]] drogon::Task<bool> notify(const MessageNotice& notice) const override;

private:
  std::shared_ptr<const NotificationService> service_;
};
