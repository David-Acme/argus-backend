#include "sync-fan-out.hxx"

#include <feature/fanout/services/audit-fan-out.hxx>
#include <shared/services/room/room-manager.hxx>
#include <string>
#include <string_view>
#include <sync/sync-change.hxx>
#include <sync/sync-operation.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <utility>
#include <vector>

namespace
{
const RoomManager roomManager;

std::string kindOf(const Json::Value& json)
{
  return json.isObject() ? json.get(sync_change::kKindField, "").asString()
                         : std::string{};
}
}

namespace sync_fan_out
{
std::optional<Event> parseEvent(const Json::Value& json)
{
  if (!json.isObject() || !json.isMember("operation") ||
      !json["operation"].isInt() || !json.isMember("option") ||
      !json["option"].isString())
    return std::nullopt;

  Event event;
  event.emit.operation = static_cast<SyncOperation>(json["operation"].asInt());
  event.emit.option = tableNameFromString(json["option"].asString());
  event.emit.obj = json["info"];

  if (json.isMember(sync_change::kUsersField) &&
      json[sync_change::kUsersField].isArray()) {
    std::vector<int64_t> users;
    for (const auto& id : json[sync_change::kUsersField])
      users.push_back(id.asInt64());
    event.users = std::move(users);
  }

  const std::string action =
      json.get(sync_change::kActionField, sync_change::kActionEmit).asString();
  if (action == sync_change::kActionDisconnect ||
      action == sync_change::kActionReplaceRoleRooms) {
    if (!json.isMember(sync_change::kUserField))
      return std::nullopt;
    event.user = json[sync_change::kUserField].asInt64();
    if (action == sync_change::kActionReplaceRoleRooms) {
      if (!json.isMember(sync_change::kOldRoleField) ||
          !json.isMember(sync_change::kNewRoleField))
        return std::nullopt;
      event.oldRole =
          userRoleFromString(json[sync_change::kOldRoleField].asString());
      event.newRole =
          userRoleFromString(json[sync_change::kNewRoleField].asString());
    }
  }
  return event;
}

FanOutPlan planEvent(const Event& event)
{
  FanOutPlan plan;
  if (event.user) {
    plan.userId = *event.user;
    if (event.oldRole) {
      plan.kind = FanOutPlan::Kind::ReplaceRoleRooms;
      plan.replaceInput = {*event.user, *event.oldRole, *event.newRole};
      return plan;
    }
    plan.kind = FanOutPlan::Kind::Disconnect;
    return plan;
  }
  if (event.users) {
    plan.kind = FanOutPlan::Kind::UserEmit;
    plan.rooms.reserve(event.users->size());
    for (const auto userId : *event.users)
      plan.rooms.push_back(userRoom(userId));
    return plan;
  }
  plan.kind = FanOutPlan::Kind::ModuleEmit;
  plan.room = moduleRoom(event.emit.option);
  return plan;
}

void dispatchEvent(const Event& event)
{
  const FanOutPlan plan = planEvent(event);
  const auto message = json_util::toString(event.emit.toJson());
  switch (plan.kind) {
    case FanOutPlan::Kind::ReplaceRoleRooms:
      roomManager.replaceRoleRooms(plan.replaceInput);
      return;
    case FanOutPlan::Kind::Disconnect:
      roomManager.disconnectUser(plan.userId, message);
      return;
    case FanOutPlan::Kind::UserEmit:
      if (!plan.rooms.empty())
        roomManager.emitMany(plan.rooms, message);
      return;
    case FanOutPlan::Kind::ModuleEmit:
      roomManager.emit(plan.room, message);
      return;
  }
}

drogon::Task<DurableDisposition>
handleChangePayload(const Json::Value& json, AuditFanOut& auditFanOut)
{
  const std::string kind = kindOf(json);
  if (kind == sync_change::kKindIdentity)
    co_return DurableDisposition::Ack;
  if (kind == sync_change::kKindAudit)
    co_return co_await auditFanOut.handleAuditChange(json)
                  ? DurableDisposition::Ack
                  : DurableDisposition::Term;

  const auto event = parseEvent(json);
  if (!event) {
    LOG_WARN << "Sync fan-out: malformed change event refused";
    co_return DurableDisposition::Term;
  }
  dispatchEvent(*event);
  co_return DurableDisposition::Ack;
}

drogon::Task<DurableDisposition> handleActionPayload(ActionPayloadInput input)
{
  co_return co_await input.auditFanOut.handleActionJournal(input.json,
                                                           input.msgId)
                ? DurableDisposition::Ack
                : DurableDisposition::Term;
}
}
