#pragma once

#include <json/value.h>
#include <optional>
#include <shared/dtos/socket-emit/socket-emit-dto.hxx>
#include <shared/enums.hxx>
#include <shared/services/room/room-manager.hxx>
#include <vector>


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

// Room routing decision for one event, the same mapping the legacy
// SocketService applies: plain emits target the module room or the user
// rooms, room-control actions take precedence over them.
struct FanOutPlan
{
  enum class Kind
  {
    ModuleEmit,
    UserEmit,
    Disconnect,
    ReplaceRoleRooms
  };
  Kind kind{Kind::ModuleEmit};
  RoomId room{0};
  std::vector<RoomId> rooms;
  int64_t userId{0};
  RoleRoomReplaceInput replaceInput{0, UserRole::Guest, UserRole::Guest};
};

std::optional<Event> parseEvent(const Json::Value& json);
FanOutPlan planEvent(const Event& event);
void dispatchEvent(const Event& event);

} // namespace sync_fan_out
