#include "nats-presence-signal-sink.hxx"

#include <json/value.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>
#include <utility>

NatsPresenceSignalSink::NatsPresenceSignalSink(std::shared_ptr<NatsBus> bus)
    : bus_(std::move(bus))
{
}

void NatsPresenceSignalSink::publish(const PresenceSignal& signal)
{
  if (!bus_)
    return;
  Json::Value payload(Json::objectValue);
  payload["userId"] = static_cast<Json::Int64>(signal.userId);
  payload["sessionId"] = signal.sessionId;
  payload["platform"] = sessionPlatformToString(signal.platform);
  payload["origin"] = sessionOriginToString(signal.origin);
  payload["at"] = static_cast<Json::Int64>(signal.at);
  bus_->publish(nats_subject::kAuthPresenceSignal,
                json_util::toString(payload));
}
