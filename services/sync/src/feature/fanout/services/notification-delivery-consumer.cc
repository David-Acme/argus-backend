#include "notification-delivery-consumer.hxx"
#include <shared/infra/notification-row-json.hxx>

#include <ctime>
#include <drogon/drogon.h>
#include <feature/fanout/services/durable-delivery.hxx>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <nats/nats-bus.hxx>
#include <sync/socket-emit-dto.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

#include <string>
#include <vector>

namespace
{
void dispatchToSockets(const NotificationDeliveryEvent& event)
{
  const Json::Value row =
      NotificationRowJson{.id = event.notificationId,
                          .userId = event.userId,
                          .type = event.type,
                          .title = event.title,
                          .body = event.body,
                          .data = event.data,
                          .isRead = false,
                          .readAt = std::nullopt,
                          .createdAt = event.createdAt}
          .toJson();

  sync_fan_out::Event fanout;
  fanout.emit.operation = SyncOperation::Add;
  fanout.emit.option = TableName::Notification;
  fanout.emit.obj = row;
  fanout.users = std::vector<int64_t>{event.userId};
  sync_fan_out::dispatchEvent(fanout);
}
}

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

void NotificationDeliveryConsumer::requestStop()
{
  stop();
}

bool NotificationDeliveryConsumer::drained() const
{
  return pending_->load(std::memory_order_acquire) == 0;
}

drogon::Task<DurableDisposition>
NotificationDeliveryConsumer::handlePayload(const std::string& payload)
{
  const Json::Value json = json_util::fromString(payload);
  if (!json.isObject()) {
    LOG_WARN << "Delivery consumer: payload is not a JSON object; refused";
    co_return DurableDisposition::Term;
  }
  if (notification_delivery::isProbe(json))
    co_return DurableDisposition::Ack;
  const auto event = NotificationDeliveryEvent::fromJson(json);
  if (!event) {
    LOG_WARN << "Delivery consumer: dropped malformed payload";
    co_return DurableDisposition::Term;
  }
  co_return co_await handle(*event);
}

drogon::Task<DurableDisposition> NotificationDeliveryConsumer::handle(
    const NotificationDeliveryEvent& event)
{
  if (event.deliveryId <= 0 || event.notificationId <= 0 || event.userId <= 0) {
    LOG_WARN << "Delivery consumer: dropped malformed delivery event";
    co_return DurableDisposition::Term;
  }
  const auto now = static_cast<int64_t>(std::time(nullptr));
  const DeliveryReceipt receipt =
      co_await repository_.receive({.event = event, .at = now});
  if (receipt.duplicate)
    co_return DurableDisposition::Ack;
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
      co_return DurableDisposition::Term;
    }
    co_return DurableDisposition::Nak;
  }
  if (!co_await repository_.markDispatched(event.deliveryId, now))
    throw std::runtime_error("delivery receipt settle failed");
  co_return DurableDisposition::Ack;
}

bool NotificationDeliveryConsumer::trySubscribe()
{
  const auto subscription = dependencies_.bus->subscribeDurable(
      {.stream = config_.stream,
       .durable = config_.durable,
       .subject = config_.subject,
       .deliverAll = true,
       .maxDeliver = config_.maxDeliver,
       .maxAckPending = NatsBus::kDefaultMaxAckPending,
       .handler = durable_delivery::handler(
           {.label = "Delivery consumer", .pending = pending_}, [this](const durable_delivery::Payload& payload) {
             return handlePayload(payload.body);
           })});
  if (!subscription)
    return false;
  subscription_ = subscription;
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
