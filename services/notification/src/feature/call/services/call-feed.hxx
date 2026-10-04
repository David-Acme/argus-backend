#pragma once

#include <feature/call/services/call-engine.hxx>
#include <json/value.h>
#include <nats/nats-bus.hxx>

#include <memory>
#include <optional>

namespace call_feed
{
std::optional<KnownSeenEvent> knownSeenFrom(const Json::Value& payload);

void subscribe(NatsBus& bus, const std::shared_ptr<const CallEngine>& engine);

void startSweep(const std::shared_ptr<const CallEngine>& engine);
}
