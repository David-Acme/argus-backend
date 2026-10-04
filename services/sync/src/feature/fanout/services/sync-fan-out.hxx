#pragma once

#include <auth/user-role.hxx>
#include <drogon/utils/coroutine.h>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <feature/fanout/services/durable-disposition.hxx>
#include <json/value.h>
#include <functional>
#include <optional>
#include <shared/services/room/room-manager.hxx>
#include <string>
#include <string_view>
#include <sync/socket-emit-dto.hxx>
#include <vector>

namespace sync_fan_out
{
struct Event
{
  SocketEmitDto emit;
  std::optional<std::vector<int64_t>> users;
  std::optional<int64_t> user;
  std::optional<std::string> session;
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
    DisconnectSession,
    ReplaceRoleRooms
  };
  Kind kind{Kind::ModuleEmit};
  RoomId room{0};
  std::vector<RoomId> rooms;
  int64_t userId{0};
  std::string sessionId;
  RoleRoomReplaceInput replaceInput{0, UserRole::Guest, UserRole::Guest};
};

struct SessionEndNotice
{
  int64_t userId{0};
  std::optional<std::string> sessionId;
};

using SessionEndListener = std::function<void(const SessionEndNotice&)>;

void onSessionEnd(SessionEndListener listener);

std::optional<Event> parseEvent(const Json::Value& json);
FanOutPlan planEvent(const Event& event);
void dispatchEvent(const Event& event);

drogon::Task<DurableDisposition> handleChangePayload(const Json::Value& json,
                                                     AuditFanOut& auditFanOut);
struct ActionPayloadInput
{
  const Json::Value& json;
  std::string_view msgId;
  AuditFanOut& auditFanOut;
};

drogon::Task<DurableDisposition> handleActionPayload(ActionPayloadInput input);

}
