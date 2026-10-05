#pragma once

#include <auth/user-role.hxx>
#include <drogon/utils/coroutine.h>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <feature/fanout/services/durable-disposition.hxx>
#include <json/value.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <shared/services/room/room-manager.hxx>
#include <string>
#include <string_view>
#include <sync/socket-emit-dto.hxx>
#include <sync/table-name.hxx>
#include <vector>

namespace sync_fan_out
{
enum class ControlScope : uint8_t
{
  None,
  Session,
  All
};

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
  std::string cause;
};

using SessionEndListener = std::function<void(const SessionEndNotice&)>;

void onSessionEnd(SessionEndListener listener);

struct IdentityChangeNotice
{
  TableName table{TableName::User};
  int64_t recordId{0};
};

using IdentityChangeListener = std::function<void(const IdentityChangeNotice&)>;

void onIdentityChange(IdentityChangeListener listener);

[[nodiscard]] ControlScope controlScopeOf(std::string_view subject);

std::optional<Event> parseEvent(const Json::Value& json,
                                ControlScope scope = ControlScope::All);
FanOutPlan planEvent(const Event& event);

struct RoomFrame
{
  RoomId room{0};
  std::string message;
};

[[nodiscard]] std::vector<RoomFrame> moduleFrames(const Event& event);
void dispatchEvent(const Event& event);

struct ChangePayloadInput
{
  const Json::Value& json;
  std::string_view subject;
  AuditFanOut& auditFanOut;
};

drogon::Task<DurableDisposition> handleChangePayload(ChangePayloadInput input);
struct ActionPayloadInput
{
  const Json::Value& json;
  std::string_view msgId;
  AuditFanOut& auditFanOut;
};

drogon::Task<DurableDisposition> handleActionPayload(ActionPayloadInput input);

}
