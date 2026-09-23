#include "nats-notification-delivery-sink.hxx"

#include <text/json-util.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <sync/stream-retention.hxx>
#include <trantor/utils/Logger.h>

NatsNotificationDeliverySink::NatsNotificationDeliverySink(
    std::shared_ptr<NatsBus> bus, Config config)
    : bus_(std::move(bus)), config_(std::move(config))
{
}

bool NatsNotificationDeliverySink::ensureStream() const
{
  if (!bus_->ensureStream({.name = config_.stream,
                           .subjects = {config_.subject},
                           .maxAgeNs = stream_retention::kRetentionNs,
                           .duplicatesNs = stream_retention::kDuplicatesNs}))
    return false;
  return true;
}

bool NatsNotificationDeliverySink::publish(
    const NotificationDeliveryEvent& event) const
{
  if (event.deliveryId <= 0)
    return false;
  const bool stored = bus_->publishWithMsgId(
      {.subject = config_.subject,
       .payload = json_util::toString(event.toJson()),
       .msgId = notification_delivery::messageId(event.deliveryId)});
  if (!stored)
    LOG_WARN << "Delivery funnel: broker refused delivery "
             << event.deliveryId << "; intent stays pending";
  return stored;
}
