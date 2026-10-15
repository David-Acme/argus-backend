#include "push-delivery-wire.hxx"

#include <text/base64.hxx>
#include <text/json-util.hxx>

#include <utility>

namespace push_delivery_wire
{
Json::Value toJson(const PushDeliveryMessage& message)
{
  const PushSealedEnvelope& envelope = message.envelope;
  Json::Value json(Json::objectValue);
  json["deliveryId"] = base64::encode(envelope.deliveryId);
  json["provider"] = std::string(pushProviderToString(envelope.provider));
  json["kind"] = std::string(pushKindToString(envelope.kind));
  json["priority"] = std::string(pushPriorityToString(envelope.priority));
  json["ttlSeconds"] = static_cast<Json::UInt64>(envelope.ttlSeconds);
  json["expiresAt"] = static_cast<Json::Int64>(message.expiresAtMs);
  json["collapseId"] = envelope.collapseId;
  json["token"] = envelope.token;
  json["enc"] = base64::encode(envelope.enc);
  json["ciphertext"] = base64::encode(envelope.ciphertext);
  return json;
}

std::string hexOf(std::string_view bytes)
{
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const char byte : bytes) {
    const auto value = static_cast<unsigned char>(byte);
    out.push_back(kDigits[value >> 4U]);
    out.push_back(kDigits[value & 0x0FU]);
  }
  return out;
}

std::string eventIdOf(const PushSealedEnvelope& envelope)
{
  return std::string(kEventIdPrefix) + hexOf(envelope.deliveryId);
}

PushDeliveryRecord recordOf(const PushDeliveryMessage& message,
                            drogon::orm::DbClient* client)
{
  return {.eventId = eventIdOf(message.envelope),
          .payload = json_util::toString(toJson(message)),
          .client = client};
}
}
