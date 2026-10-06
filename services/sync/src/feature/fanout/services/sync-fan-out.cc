#include "sync-fan-out.hxx"

#include <camera/camera-row-projection.hxx>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <shared/services/room/room-manager.hxx>
#include <string>
#include <string_view>
#include <sync/sync-change.hxx>
#include <json/value.h>
#include <nats/nats-subject.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <utility>
#include <vector>

namespace
{
const RoomManager roomManager;

constexpr std::size_t kMaxSessionIdBytes = 64;

std::string causeOf(const Json::Value& info)
{
  constexpr std::size_t kMaxCauseBytes = 32;
  if (!info.isObject() || !info["cause"].isString())
    return {};
  std::string cause = info["cause"].asString();
  return cause.size() <= kMaxCauseBytes ? cause : std::string();
}

sync_fan_out::SessionEndListener& sessionEndListener()
{
  static sync_fan_out::SessionEndListener listener;
  return listener;
}

sync_fan_out::IdentityChangeListener& identityChangeListener()
{
  static sync_fan_out::IdentityChangeListener listener;
  return listener;
}

void noticeIdentityChange(const Json::Value& json)
{
  if (!identityChangeListener() || !json[sync_change::kTableField].isString() ||
      !json[sync_change::kRecordIdField].isIntegral())
    return;
  const auto table = findTableName(json[sync_change::kTableField].asString());
  const int64_t recordId = json[sync_change::kRecordIdField].asInt64();
  if (!table || recordId <= 0)
    return;
  identityChangeListener()({.table = *table, .recordId = recordId});
}

constexpr const char* kSettledField = "settled";
constexpr const char* kModuleField = "module";
constexpr const char* kModulesField = "modules";
constexpr const char* kModuleIdField = "id";
constexpr const char* kModuleEnabledField = "enabled";
constexpr Json::ArrayIndex kMaxModules = 64;

std::string moduleUpdateMessage(const Json::Value& info)
{
  SocketEmitDto frame;
  frame.operation = SyncOperation::ModuleUpdate;
  frame.option = TableName::User;
  frame.obj = info;
  return json_util::toString(frame.toJson());
}

constexpr const char* kDataPurgedAtField = "dataPurgedAt";
constexpr const char* kLifecycleField = "lifecycle";

Json::Value purgeNoticeOf(const Json::Value& module)
{
  Json::Value notice(Json::objectValue);
  for (const char* field : {kModuleIdField, kModuleEnabledField, kLifecycleField, kDataPurgedAtField}) {
    if (module.isMember(field))
      notice[field] = module[field];
  }
  return notice;
}

std::optional<Json::Value> enabledSetOf(const Json::Value& modules)
{
  if (modules.size() > kMaxModules)
    return std::nullopt;
  Json::Value list(Json::arrayValue);
  for (const auto& entry : modules) {
    if (!entry.isObject() || !entry[kModuleIdField].isString() ||
        !entry[kModuleEnabledField].isBool())
      return std::nullopt;
    Json::Value flag(Json::objectValue);
    flag[kModuleIdField] = entry[kModuleIdField];
    flag[kModuleEnabledField] = entry[kModuleEnabledField];
    if (entry[kLifecycleField].isString())
      flag[kLifecycleField] = entry[kLifecycleField];
    list.append(std::move(flag));
  }
  Json::Value info(Json::objectValue);
  info[kModulesField] = std::move(list);
  return info;
}

std::string kindOf(const Json::Value& json)
{
  return json.isObject() ? json.get(sync_change::kKindField, "").asString()
                         : std::string{};
}
}

namespace sync_fan_out
{
void onSessionEnd(SessionEndListener listener)
{
  sessionEndListener() = std::move(listener);
}

void onIdentityChange(IdentityChangeListener listener)
{
  identityChangeListener() = std::move(listener);
}

ControlScope controlScopeOf(std::string_view subject)
{
  return subject == nats_subject::kAuthSession ? ControlScope::Session
                                               : ControlScope::None;
}

std::optional<Event> parseEvent(const Json::Value& json, ControlScope scope)
{
  if (!json.isObject() || !json.isMember("operation") ||
      !json["operation"].isInt() || !json.isMember("option") ||
      !json["option"].isString())
    return std::nullopt;

  const auto table = findTableName(json["option"].asString());
  if (!table)
    return std::nullopt;

  Event event;
  event.emit.operation = static_cast<SyncOperation>(json["operation"].asInt());
  if (event.emit.operation == SyncOperation::ModuleUpdate)
    return std::nullopt;
  event.emit.option = *table;
  event.emit.obj = json["info"];

  if (json.isMember(sync_change::kUsersField) &&
      json[sync_change::kUsersField].isArray()) {
    std::vector<int64_t> users;
    for (const auto& id : json[sync_change::kUsersField]) {
      if (!id.isInt64())
        return std::nullopt;
      if (id.asInt64() > 0)
        users.push_back(id.asInt64());
    }
    event.users = std::move(users);
  }

  const std::string action =
      json.get(sync_change::kActionField, sync_change::kActionEmit).asString();
  if (action == sync_change::kActionDisconnectSession) {
    if (scope == ControlScope::None)
      return std::nullopt;
    if (!json.isMember(sync_change::kUserField) ||
        !json[sync_change::kUserField].isInt64() ||
        json[sync_change::kUserField].asInt64() <= 0 ||
        !json.isMember(sync_change::kSessionField) ||
        !json[sync_change::kSessionField].isString())
      return std::nullopt;
    std::string session = json[sync_change::kSessionField].asString();
    if (session.empty() || session.size() > kMaxSessionIdBytes)
      return std::nullopt;
    event.user = json[sync_change::kUserField].asInt64();
    event.session = std::move(session);
    return event;
  }
  if (action == sync_change::kActionDisconnect ||
      action == sync_change::kActionReplaceRoleRooms) {
    if (scope != ControlScope::All)
      return std::nullopt;
    if (!json.isMember(sync_change::kUserField) ||
        !json[sync_change::kUserField].isInt64() ||
        json[sync_change::kUserField].asInt64() <= 0)
      return std::nullopt;
    event.user = json[sync_change::kUserField].asInt64();
    if (action == sync_change::kActionReplaceRoleRooms) {
      if (!json.isMember(sync_change::kOldRoleField) ||
          !json.isMember(sync_change::kNewRoleField) ||
          !json[sync_change::kOldRoleField].isString() ||
          !json[sync_change::kNewRoleField].isString())
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
    if (event.session) {
      plan.kind = FanOutPlan::Kind::DisconnectSession;
      plan.sessionId = *event.session;
      return plan;
    }
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

std::vector<RoomFrame> moduleFrames(const Event& event)
{
  const RoomId room = moduleRoom(event.emit.option);
  std::vector<RoomFrame> frames;
  frames.push_back({.room = room, .message = json_util::toString(event.emit.toJson())});
  if (event.emit.option != TableName::Camera)
    return frames;
  SocketEmitDto reduced = event.emit;
  if (reduced.operation == SyncOperation::Log)
    camera_projection::reduceDiff(reduced.obj["changes"]);
  else if (reduced.operation != SyncOperation::Delete)
    camera_projection::reduceRow(reduced.obj);
  frames.push_back({.room = reducedModuleRoom(event.emit.option),
                    .message = json_util::toString(reduced.toJson())});
  return frames;
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
      if (sessionEndListener())
        sessionEndListener()({.userId = plan.userId, .sessionId = std::nullopt, .cause = "accountDisabled"});
      return;
    case FanOutPlan::Kind::DisconnectSession:
      roomManager.disconnectSession({.userId = plan.userId,
                                     .sessionId = plan.sessionId,
                                     .contextMessage = message,
                                     .cause = causeOf(event.emit.obj)});
      if (sessionEndListener())
        sessionEndListener()({.userId = plan.userId,
                              .sessionId = plan.sessionId,
                              .cause = causeOf(event.emit.obj)});
      return;
    case FanOutPlan::Kind::UserEmit:
      if (!plan.rooms.empty())
        roomManager.emitMany(plan.rooms, message);
      return;
    case FanOutPlan::Kind::ModuleEmit:
      for (const auto& frame : moduleFrames(event))
        roomManager.emit(frame.room, frame.message);
      return;
  }
}

bool PurgeWatermarks::advance(const std::string& module, const Json::Value& purgedAt)
{
  const bool comparable = purgedAt.isIntegral() || (purgedAt.isString() && !purgedAt.asString().empty());
  if (!comparable)
    return false;
  std::scoped_lock lock(mutex_);
  const auto found = seen_.find(module);
  if (found != seen_.end() && found->second.type() == purgedAt.type()) {
    const bool newer = purgedAt.isIntegral() ? purgedAt.asInt64() > found->second.asInt64()
                                             : purgedAt.asString() > found->second.asString();
    if (!newer)
      return false;
  }
  seen_[module] = purgedAt;
  return true;
}

std::vector<RoomFrame> moduleUpdateFrames(const Json::Value& json, PurgeWatermarks& purges)
{
  std::vector<RoomFrame> frames;
  if (!json.isObject())
    return frames;
  const Json::Value& settled = json[kSettledField];
  const bool unsettled = settled.isBool() && !settled.asBool();
  const Json::Value& module = json[kModuleField];
  if (module.isObject() && module[kModuleIdField].isString()) {
    frames.push_back({.room = roleRoom(UserRole::Owner),
                      .message = moduleUpdateMessage(module)});
    if (purges.advance(module[kModuleIdField].asString(), module[kDataPurgedAtField]))
      frames.push_back({.room = kConnectedRoom, .message = moduleUpdateMessage(purgeNoticeOf(module))});
  }
  const Json::Value& modules = json[kModulesField];
  if (modules.isArray() && !unsettled) {
    if (const auto enabledSet = enabledSetOf(modules))
      frames.push_back({.room = kConnectedRoom,
                        .message = moduleUpdateMessage(*enabledSet)});
  }
  return frames;
}

DurableDisposition handleModulePayload(const Json::Value& json)
{
  if (!json.isObject())
    return DurableDisposition::Term;
  static PurgeWatermarks purges;
  const auto frames = moduleUpdateFrames(json, purges);
  const Json::Value& settled = json[kSettledField];
  if (frames.empty() && !(settled.isBool() && !settled.asBool())) {
    LOG_WARN << "Sync fan-out: a module event with neither a module nor an enabled set refused";
    return DurableDisposition::Term;
  }
  for (const auto& frame : frames)
    roomManager.emit(frame.room, frame.message);
  return DurableDisposition::Ack;
}

drogon::Task<DurableDisposition> handleChangePayload(ChangePayloadInput input)
{
  const Json::Value& json = input.json;
  try {
    const std::string kind = kindOf(json);
    if (kind == sync_change::kKindIdentity) {
      if (input.subject == nats_subject::kIdentityChange)
        noticeIdentityChange(json);
      co_return DurableDisposition::Ack;
    }
    if (kind == sync_change::kKindAudit)
      co_return co_await input.auditFanOut.handleAuditChange(json)
                    ? DurableDisposition::Ack
                    : DurableDisposition::Term;

    const auto event = parseEvent(json, controlScopeOf(input.subject));
    if (!event) {
      LOG_WARN << "Sync fan-out: malformed or unauthorised change event on "
               << input.subject << " refused";
      co_return DurableDisposition::Term;
    }
    dispatchEvent(*event);
    co_return DurableDisposition::Ack;
  }
  catch (const Json::Exception& error) {
    LOG_WARN << "Sync fan-out: change event with a malformed field refused: "
             << error.what();
    co_return DurableDisposition::Term;
  }
}

drogon::Task<DurableDisposition> handleActionPayload(ActionPayloadInput input)
{
  try {
    co_return co_await input.auditFanOut.handleActionJournal(input.json,
                                                             input.msgId)
                  ? DurableDisposition::Ack
                  : DurableDisposition::Term;
  }
  catch (const Json::Exception& error) {
    LOG_WARN << "Action journal: event with a malformed field refused: "
             << error.what();
    co_return DurableDisposition::Term;
  }
}
}
