#include "nats-presence-publisher.hxx"

#include <json/value.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <text/json-util.hxx>

NatsPresencePublisher::NatsPresencePublisher(NatsBus* bus) : bus_(bus) {}

void NatsPresencePublisher::publish(const PresenceChange& change)
{
  if (bus_ == nullptr)
    return;
  Json::Value payload(Json::objectValue);
  payload["userId"] = static_cast<Json::Int64>(change.row.userId);
  payload["environmentId"] = static_cast<Json::Int64>(change.row.environmentId);
  payload["state"] = presenceStateToString(change.row.state);
  payload["source"] = presenceSourceToString(change.row.source);
  payload["since"] = static_cast<Json::Int64>(change.row.since);
  payload["overall"] = presenceStateToString(change.overall);
  bus_->publish(nats_subject::kGuardPresenceChanged,
                json_util::toString(payload));
}
