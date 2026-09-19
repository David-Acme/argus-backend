#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>

// Durable per-recipient delivery event for argus.notification.v1.delivery.
// The deliveryId is the stable identity: the notification service publishes
// each pending delivery intent exactly under it, and the gateway-side inbox
// receipts it before dispatching.
struct NotificationDeliveryEvent
{
  int64_t deliveryId{0};
  int64_t notificationId{0};
  int64_t userId{0};
  std::string type;
  std::string title;
  std::string body;
  Json::Value data;
  int64_t createdAt{0};

  Json::Value toJson() const
  {
    Json::Value json(Json::objectValue);
    json["deliveryId"] = Json::Int64(deliveryId);
    json["notificationId"] = Json::Int64(notificationId);
    json["userId"] = Json::Int64(userId);
    json["type"] = type;
    json["title"] = title;
    json["body"] = body;
    json["data"] = data.isNull() ? Json::Value(Json::objectValue) : data;
    json["createdAt"] = Json::Int64(createdAt);
    return json;
  }

  static std::optional<NotificationDeliveryEvent> fromJson(
      const Json::Value& json)
  {
    if (!json.isObject())
      return std::nullopt;
    NotificationDeliveryEvent event;
    event.deliveryId = json.get("deliveryId", 0).asInt64();
    event.notificationId = json.get("notificationId", 0).asInt64();
    event.userId = json.get("userId", 0).asInt64();
    if (event.deliveryId <= 0 || event.notificationId <= 0 ||
        event.userId <= 0)
      return std::nullopt;
    event.type = json.get("type", "").asString();
    event.title = json.get("title", "").asString();
    event.body = json.get("body", "").asString();
    event.data = json.get("data", Json::Value(Json::objectValue));
    event.createdAt = json.get("createdAt", 0).asInt64();
    return event;
  }
};

// Durable delivery sink owned by argus-notification and injected into the
// notification service; never a process-global.
class NotificationDeliverySink
{
public:
  virtual ~NotificationDeliverySink() = default;

  // Reconciles the notification-owned stream; false leaves every intent
  // pending without attempting a publish.
  virtual bool ensureStream() const = 0;

  // True only after the broker stored the event (JetStream PubAck). False
  // leaves the delivery intent pending for the reconciler.
  virtual bool publish(const NotificationDeliveryEvent& event) const = 0;
};

namespace notification_delivery
{
// Deterministic JetStream message id: redeliveries of one intent dedup.
inline std::string messageId(int64_t deliveryId)
{
  return "notification-delivery:" + std::to_string(deliveryId);
}
} // namespace notification_delivery
