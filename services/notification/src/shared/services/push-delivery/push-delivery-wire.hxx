#pragma once

#include <shared/services/push-delivery/push-delivery-outbox.hxx>

#include <push/push-envelope.hxx>

#include <json/value.h>

#include <cstdint>
#include <string>
#include <string_view>

struct PushDeliveryMessage
{
  PushSealedEnvelope envelope;
  int64_t expiresAtMs{0};
};

namespace push_delivery_wire
{
inline constexpr std::string_view kEventIdPrefix = "push-delivery:";

[[nodiscard]] Json::Value toJson(const PushDeliveryMessage& message);

[[nodiscard]] std::string hexOf(std::string_view bytes);

[[nodiscard]] std::string eventIdOf(const PushSealedEnvelope& envelope);

[[nodiscard]] PushDeliveryRecord recordOf(const PushDeliveryMessage& message,
                                          drogon::orm::DbClient* client);
}
