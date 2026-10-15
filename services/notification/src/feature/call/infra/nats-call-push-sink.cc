#include "nats-call-push-sink.hxx"

#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/base64.hxx>
#include <text/json-util.hxx>

#include <json/value.h>

#include <utility>

Json::Value call_push_wire::toJson(const CallPushMessage& message)
{
  const PushSealedEnvelope& envelope = message.envelope;
  Json::Value json(Json::objectValue);
  json["deliveryId"] = base64::encode(envelope.deliveryId);
  json["provider"] = std::string(pushProviderToString(envelope.provider));
  json["kind"] = std::string(pushKindToString(envelope.kind));
  json["priority"] = std::string(pushPriorityToString(envelope.priority));
  json["ttlSeconds"] = Json::UInt64(envelope.ttlSeconds);
  json["expiresAt"] = Json::Int64(message.expiresAtMs);
  json["collapseId"] = envelope.collapseId;
  json["token"] = envelope.token;
  json["enc"] = base64::encode(envelope.enc);
  json["ciphertext"] = base64::encode(envelope.ciphertext);
  return json;
}

NatsCallPushSink::NatsCallPushSink(std::shared_ptr<NatsBus> bus) : bus_(std::move(bus)) {}

bool NatsCallPushSink::publish(const CallPushMessage& message) const
{
  if (!bus_)
    return false;
  return bus_->publish(nats_subject::kNotificationPushDelivery,
                       json_util::toString(call_push_wire::toJson(message)));
}
