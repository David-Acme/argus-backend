#pragma once

#include <json/value.h>
#include <memory>
#include <optional>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <vector>

class NatsBus;

// Parsed `argus.*.v1.change` event (see shared/services/socket/sync-change.hxx
// for the wire contract): the emit triple plus the routing metadata the
// gateway consumes and never re-emits.
namespace sync_fan_out
{
struct Event
{
  SocketEmitDto emit;
  std::optional<std::vector<int64_t>> users;
  std::optional<int64_t> user;
  std::optional<UserRole> oldRole;
  std::optional<UserRole> newRole;
};

std::optional<Event> parseEvent(const Json::Value& json);
void dispatchEvent(const Event& event);

// Subscribes the tail-only sync-change wildcard and re-emits every event
// through the gateway's RoomManager exactly as the legacy SocketService would.
// The gateway never publishes: only the legacy installs the event bus.
void subscribeSyncFanOut(NatsBus& bus);
} // namespace sync_fan_out