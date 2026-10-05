#pragma once

#include <feature/call/services/call-engine.hxx>
#include <json/value.h>
#include <nats/nats-bus.hxx>
#include <shared/services/task-gate/task-gate.hxx>

#include <memory>
#include <optional>
#include <string>

namespace call_feed
{
inline constexpr const char* kKnownSeenDurable = "notification-known-seen";

std::optional<KnownSeenEvent> knownSeenFrom(const Json::Value& payload);

struct FeedInput
{
  std::shared_ptr<NatsBus> bus;
  std::shared_ptr<const CallEngine> engine;
  std::shared_ptr<TaskGate> tasks;
};

void subscribe(const FeedInput& input);

struct SweepInput
{
  std::shared_ptr<const CallEngine> engine;
  std::shared_ptr<TaskGate> tasks;
};

void startSweep(const SweepInput& input);
}
