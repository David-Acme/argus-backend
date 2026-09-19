#include "nats-notification-delivery-sink.hxx"

#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

NatsNotificationDeliverySink::NatsNotificationDeliverySink(
    std::shared_ptr<NatsBus> bus, Config config)
    : bus_(std::move(bus)), config_(std::move(config))
{
}

bool NatsNotificationDeliverySink::ensureStream() const
{
  constexpr int64_t kRetentionNs = 7LL * 24 * 60 * 60 * 1000000000;
  constexpr int64_t kDuplicatesNs = 2LL * 60 * 1000000000;
  if (!bus_->ensureStream({.name = config_.stream,
                           .subjects = {config_.subject},
                           .maxAgeNs = kRetentionNs,
                           .duplicatesNs = kDuplicatesNs}))
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
