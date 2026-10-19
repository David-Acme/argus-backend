#include "notification-message-notice.hxx"

#include <utility>

namespace
{
constexpr const char* kHouseholdMessageType = "household_message";
}

NotificationMessageNotice::NotificationMessageNotice(std::shared_ptr<const NotificationService> notifications)
    : service_(std::move(notifications))
{
}

drogon::Task<bool> NotificationMessageNotice::notify(const MessageNotice& notice) const
{
  if (!service_)
    co_return false;
  const NotificationBatchInput batch{.userIds = {notice.userId},
                                     .notification = {.userId = notice.userId,
                                                      .type = kHouseholdMessageType,
                                                      .title = notice.title,
                                                      .body = notice.body,
                                                      .data = notice.data},
                                     .commandId = notice.commandId};
  const NotificationCreateOutcome outcome = co_await service_->createManyAndEmit(batch);
  co_return outcome.createdCount > 0 || outcome.duplicate;
}
