#include "notification-delivery-consumer.hxx"

#include <ctime>
#include <drogon/drogon.h>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <shared/schemas/notification/notification-schema.hxx>
#include <shared/services/socket/sync-change.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <sync/user-change-fan-out.hxx>
#include <trantor/utils/Logger.h>

namespace
{
// Same row the legacy change funnel fanned out, rebuilt from the event.
void dispatchToSockets(const NotificationDeliveryEvent& event)
{
  NotificationSchema notification;
  notification.id = event.notificationId;
  notification.userId = event.userId;
  notification.type = event.type;
  notification.title = event.title;
  notification.body = event.body;
  notification.data = event.data;
  notification.createdAt = event.createdAt;

  SocketEmitDto emit;
  emit.operation = SyncOperation::Add;
  emit.option = TableName::Notification;
  emit.obj = notification.toJson();
  user_change_fan_out::handleUserChange(
      sync_change::userEmitPayload(emit, {event.userId}));
}
} // namespace

NotificationDeliveryConsumer::NotificationDeliveryConsumer(
    Dependencies dependencies, Config config)
    : dependencies_(std::move(dependencies)), config_(std::move(config))
{
  if (!dependencies_.dispatch)
    dependencies_.dispatch = dispatchToSockets;
}

NotificationDeliveryConsumer::~NotificationDeliveryConsumer()
{
  stop();
}

void NotificationDeliveryConsumer::start()
{
  if (dependencies_.bus == nullptr)
    return;
  if (trySubscribe())
    return;
  LOG_WARN << "Delivery consumer: stream not ready; retrying";
  scheduleSubscribeRetry();
}

void NotificationDeliveryConsumer::stop()
{
  if (retryTimer_.has_value()) {
    if (drogon::app().isRunning())
      drogon::app().getLoop()->invalidateTimer(*retryTimer_);
    retryTimer_.reset();
  }
  if (subscription_.has_value() && dependencies_.bus != nullptr)
    dependencies_.bus->unsubscribe(*subscription_);
  subscription_.reset();
}

drogon::Task<DeliveryDisposition>
NotificationDeliveryConsumer::handlePayload(const std::string& payload)
{
  const auto event =
      NotificationDeliveryEvent::fromJson(json_util::fromString(payload));
  if (!event) {
    LOG_WARN << "Delivery consumer: dropped malformed payload";
    co_return DeliveryDisposition::Term;
  }
  co_return co_await handle(*event);
}

drogon::Task<DeliveryDisposition> NotificationDeliveryConsumer::handle(
    const NotificationDeliveryEvent& event)
{
  if (event.deliveryId <= 0 || event.notificationId <= 0 || event.userId <= 0) {
    LOG_WARN << "Delivery consumer: dropped malformed delivery event";
    co_return DeliveryDisposition::Term;
  }
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const DeliveryReceipt receipt =
      co_await repository_.receive({.event = event, .at = now});
  if (receipt.duplicate)
    co_return DeliveryDisposition::Ack;
  std::string dispatchError;
  try {
    dependencies_.dispatch(event);
  }
  catch (const std::exception& error) {
    dispatchError = error.what();
  }
  if (!dispatchError.empty()) {
    const int64_t attempts =
        co_await repository_.noteAttempt(event.deliveryId, now);
    LOG_WARN << "Delivery consumer: dispatch failed for delivery "
             << event.deliveryId << " (attempt " << attempts
             << "): " << dispatchError;
    if (attempts >= 0 && attempts >= config_.poisonMaxAttempts &&
        co_await repository_.markDeadLettered(event.deliveryId, now)) {
      LOG_ERROR << "Delivery consumer: dead-lettered poison delivery "
                << event.deliveryId;
      co_return DeliveryDisposition::Term;
    }
    co_return DeliveryDisposition::Nak;
  }
  if (!co_await repository_.markDispatched(event.deliveryId, now))
    throw std::runtime_error("delivery receipt settle failed");
  co_return DeliveryDisposition::Ack;
}

bool NotificationDeliveryConsumer::trySubscribe()
{
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = config_.stream,
       .durable = config_.durable,
       .subject = config_.subject,
       .deliverAll = true,
       .maxDeliver = config_.maxDeliver,
       .handler = [this](const NatsBus::DurableMessage& message,
                         NatsBus::DurableSettlement settlement) {
         const std::string payload(message.payload);
         drogon::app().getIOLoop(0)->runInLoop(
             [this, payload = std::move(payload),
              settlement = std::move(settlement)]() mutable {
               drogon::async_run(
                   [this, payload = std::move(payload),
                    settlement = std::move(settlement)]() mutable
                       -> drogon::Task<void> {
                     DeliveryDisposition disposition =
                         DeliveryDisposition::Nak;
                     try {
                       disposition = co_await handlePayload(payload);
                     }
                     catch (const std::exception& error) {
                       LOG_WARN << "Delivery consumer: redelivering ("
                                << error.what() << ")";
                       disposition = DeliveryDisposition::Nak;
                     }
                     if (disposition == DeliveryDisposition::Term) {
                       if (settlement.term)
                         settlement.term();
                     }
                     else if (disposition == DeliveryDisposition::Ack) {
                       if (settlement.ack)
                         settlement.ack();
                     }
                     else if (settlement.nak) {
                       settlement.nak();
                     }
                     co_return;
                   });
             });
       }});
  if (!subscription)
    return false;
  subscription_ = *subscription;
  return true;
}

void NotificationDeliveryConsumer::scheduleSubscribeRetry()
{
  if (retryTimer_.has_value())
    return;
  retryTimer_ = drogon::app().getLoop()->runEvery(5.0, [this]() {
    if (subscription_.has_value() || dependencies_.bus == nullptr)
      return;
    if (trySubscribe())
      LOG_INFO << "Delivery consumer: durable consumer connected";
  });
}
