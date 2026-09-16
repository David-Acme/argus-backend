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

enum class DeliverPendingOutcome
{
  Settled,
  NoSinkInstalled,
  StreamUnavailable,
  PublishRefused
};

inline std::string deliverPendingOutcomeToString(DeliverPendingOutcome outcome)
{
  switch (outcome) {
  case DeliverPendingOutcome::Settled:
    return "settled";
  case DeliverPendingOutcome::NoSinkInstalled:
    return "no_sink_installed";
  case DeliverPendingOutcome::StreamUnavailable:
    return "stream_unavailable";
  case DeliverPendingOutcome::PublishRefused:
    return "publish_refused";
  }
  return "settled";
}

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

  bool hasDeliverySink() const
  {
    return dependencies_.deliverySink != nullptr;
  }

  // Drain pending intents, naming a missing sink instead of throwing.
  [[nodiscard]] drogon::Task<DeliverPendingOutcome> deliverPending() const;

  drogon::Task<int64_t> pendingBacklog() const;

  drogon::Task<void> markAsRead(int64_t userId,
                                const std::vector<int64_t>& ids) const;

  drogon::Task<int64_t> ackDeliveries(
      int64_t userId, const std::vector<int64_t>& notificationIds) const;

  drogon::Task<DeliverySummary> deliverySummary(int64_t since,
                                                int64_t ackWindowS) const;

  drogon::Task<SelfTestState> runSelfTest() const;

private:
  struct DeliverDurableInput
  {
    std::vector<NotificationDeliveryRow> pending;
    const NotificationDeliverySink& sink;
    std::shared_ptr<const push_intent::PushIntentSink> pushSink;
    bool pushRequired{false};
  };

  // Settles broker-stored intents; the rest stay pending for the reconciler.
  drogon::Task<bool> deliverDurable(DeliverDurableInput input) const;

  Dependencies dependencies_;
  NotificationRepository repository_;
};
