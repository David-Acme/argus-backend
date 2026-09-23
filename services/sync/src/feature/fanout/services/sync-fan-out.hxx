#pragma once

#include <auth/user-role.hxx>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <json/value.h>
#include <optional>
#include <shared/services/room/room-manager.hxx>
#include <sync/socket-emit-dto.hxx>
#include <vector>

class NatsBus;

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

void subscribeChangeFanOut(NatsBus& bus, AuditFanOut& auditFanOut);
void subscribeActionJournal(NatsBus& bus, AuditFanOut& auditFanOut);

}
