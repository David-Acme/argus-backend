#include "sync-fan-out.hxx"

#include <drogon/drogon.h>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <functional>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
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
// Stateless handle over the room module's thread_local registries, the same
// object the engine's SyncService holds for the process's life.
const RoomManager roomManager;

// cnats dispatcher thread: marshal into the Drogon loop first.
void inLoop(std::string_view message,
            std::function<void(const Json::Value&)> handle)
{
  drogon::app().getIOLoop(0)->runInLoop(
      [payload = std::string(message), handle = std::move(handle)]() {
        handle(json_util::fromString(payload));
      });
}

std::string kindOf(const Json::Value& json)
{
  return json.isObject() ? json.get(sync_change::kKindField, "").asString()
                         : std::string{};
}
} // namespace

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

void subscribeChangeFanOut(NatsBus& bus, AuditFanOut& auditFanOut)
{
  bus.subscribe(nats_subject::kSyncChangeWildcard,
                [fanOut = &auditFanOut](std::string_view,
                                        std::string_view message) {
                  inLoop(message, [fanOut](const Json::Value& json) {
                    const std::string kind = kindOf(json);
                    if (kind == sync_change::kKindIdentity)
                      return;
                    if (kind == sync_change::kKindAudit) {
                      fanOut->handleAuditChange(json);
                      return;
                    }
                    const auto event = parseEvent(json);
                    if (!event) {
                      LOG_WARN
                          << "Sync fan-out: dropped malformed change event";
                      return;
                    }
                    dispatchEvent(*event);
                  });
                });
}

void subscribeActionJournal(NatsBus& bus, AuditFanOut& auditFanOut)
{
  bus.subscribe(nats_subject::kIdentityUserAction,
                [fanOut = &auditFanOut](std::string_view,
                                        std::string_view message) {
                  inLoop(message, [fanOut](const Json::Value& json) {
                    fanOut->handleActionJournal(json);
                  });
                });
}
} // namespace sync_fan_out
