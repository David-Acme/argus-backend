#include "nats-identity-change-sink.hxx"

#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

NatsIdentityChangeSink::NatsIdentityChangeSink(std::shared_ptr<NatsBus> bus)
    : bus_(std::move(bus))
{
}

void NatsIdentityChangeSink::publish(const IdentityChangeInput& input) const
{
  Json::Value event(Json::objectValue);
  event["kind"] = "identity";
  event["table"] = input.table;
  event["id"] = static_cast<Json::Int64>(input.id);
  event["deleted"] = input.deleted;
  event["row"] = input.row;

  if (!bus_->publish(nats_subject::kIdentityChange,
                     json_util::toString(event)))
    LOG_WARN << "Identity funnel: publish failed for " << input.table << " "
             << input.id;
}
