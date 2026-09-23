#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>

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

class NotificationDeliverySink
{
public:
  virtual ~NotificationDeliverySink() = default;

  virtual bool ensureStream() const = 0;

  virtual bool publish(const NotificationDeliveryEvent& event) const = 0;
};

namespace notification_delivery
{
inline std::string messageId(int64_t deliveryId)
{
  return "notification-delivery:" + std::to_string(deliveryId);
}
}
