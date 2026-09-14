#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <shared/contracts/push-intent-sink.hxx>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/contracts/user-change-sink.hxx>
#include <memory>
#include <vector>

struct NotificationBatchInput
{
  std::vector<int64_t> userIds;
  NotificationCreateInput notification;
  std::string commandId;
};

struct NotificationCreateOutcome
{
  bool duplicate{false};
  int64_t createdCount{0};
};

class NotificationService
{
public:
  struct Dependencies
  {
    std::shared_ptr<const NotificationDeliverySink> deliverySink;
    std::shared_ptr<const push_intent::PushIntentSink> pushSink;
    bool pushRequired{false};
  };

  NotificationService() = default;
  explicit NotificationService(Dependencies dependencies);

  drogon::Task<NotificationCreateOutcome> createManyAndEmit(
      const NotificationBatchInput& input) const;

  drogon::Task<void> deliverPending() const;

  drogon::Task<void> markAsRead(int64_t userId,
                                const std::vector<int64_t>& ids) const;

private:
  struct DeliverDurableInput
  {
    std::vector<NotificationDeliveryRow> pending;
    const NotificationDeliverySink& sink;
    std::shared_ptr<const push_intent::PushIntentSink> pushSink;
    bool pushRequired{false};
  };

  // Publishes each intent through the durable sink; only broker-stored
  // intents are settled, the rest stay pending for the reconciler. True when
  // at least one intent settled, so the caller stops when no progress is
  // made instead of spinning on refused rows.
  drogon::Task<bool> deliverDurable(DeliverDurableInput input) const;

  Dependencies dependencies_;
  NotificationRepository repository_;
};
