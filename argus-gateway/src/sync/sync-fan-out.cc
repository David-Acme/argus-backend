#include "sync-fan-out.hxx"

#include <drogon/drogon.h>
#include <shared/contracts/sync-operation.hxx>
#include <shared/services/room/room-manager.hxx>
#include <shared/services/socket/sync-change.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

#include <string>
#include <utility>

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
  RoomManager manager;
  const auto message = json_util::toString(event.emit.toJson());
  switch (plan.kind) {
  case FanOutPlan::Kind::ReplaceRoleRooms:
    manager.replaceRoleRooms(plan.replaceInput);
    return;
  case FanOutPlan::Kind::Disconnect:
    manager.disconnectUser(plan.userId, message);
    return;
  case FanOutPlan::Kind::UserEmit:
    if (!plan.rooms.empty())
      manager.emitMany(plan.rooms, message);
    return;
  case FanOutPlan::Kind::ModuleEmit:
    manager.emit(plan.room, message);
    return;
  }
}

void subscribeSyncFanOut(NatsBus& bus)
{
  bus.subscribe(
      nats_subject::kSyncChangeWildcard,
      [](std::string_view, std::string_view message) {
        // cnats dispatcher thread: marshal the whole handler into the
        // Drogon loop before touching room state.
        drogon::app().getIOLoop(0)->runInLoop(
            [payload = std::string(message)]() {
              const Json::Value json = json_util::fromString(payload);
              const auto event = parseEvent(json);
              if (!event) {
                LOG_WARN << "Sync fan-out: dropped malformed change event";
                return;
              }
              dispatchEvent(*event);
            });
      });
}
} // namespace sync_fan_out
