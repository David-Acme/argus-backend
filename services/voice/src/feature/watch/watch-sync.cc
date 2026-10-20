#include "watch-sync.hxx"

#include <auth/user-role.hxx>
#include <sync/socket-emit-dto.hxx>
#include <trantor/utils/Logger.h>

#include <json/writer.h>


namespace
{

constexpr const char* kDevice = "argus-voice";

std::string compact(const Json::Value& value)
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

std::string commandIdOf(const std::string& type, const std::string& conversationId, int64_t userId)
{
  return type + "-" + conversationId + "-" + std::to_string(userId);
}

std::string userWords(const ConversationSnapshot& snapshot, std::string_view role)
{
  std::string out;
  for (const ConversationTurn& turn : snapshot.turns) {
    if (turn.role != role)
      continue;
    if (!out.empty())
      out += ' ';
    out += turn.text;
  }
  return out;
}

}

Json::Value watchUpdateInfo(const ConversationSnapshot& snapshot)
{
  Json::Value info(Json::objectValue);
  info["conversationId"] = snapshot.conversationId;
  info["cameraId"] = Json::Int64{snapshot.cameraId};
  info["cameraName"] = snapshot.cameraName;
  info["state"] = std::string(conversationStateToString(snapshot.state));
  info["handoff"] = std::string(conversationHandoffToString(snapshot.handoff));
  info["audience"] = std::string(conversationAudienceToString(snapshot.audience));
  info["startedAt"] = Json::Int64{snapshot.startedAt};
  info["updatedAt"] = Json::Int64{snapshot.updatedAt};
  info["resumableUntil"] =
      snapshot.resumableUntil > 0 ? Json::Value(Json::Int64{snapshot.resumableUntil}) : Json::Value();
  info["endReason"] = snapshot.endReason == ConversationEnd::None
                          ? Json::Value()
                          : Json::Value(std::string(conversationEndToString(snapshot.endReason)));
  Json::Value turns(Json::arrayValue);
  for (const ConversationTurn& turn : snapshot.turns) {
    Json::Value entry(Json::objectValue);
    entry["id"] = Json::Int64{turn.id};
    entry["role"] = turn.role;
    entry["text"] = turn.text;
    entry["at"] = Json::Int64{turn.at};
    entry["final"] = turn.final;
    turns.append(entry);
  }
  info["turns"] = std::move(turns);
  return info;
}

std::vector<WatchNotice> watchNoticesFor(const ConversationEnded& ended, VoiceLang lang,
                                         const std::vector<int64_t>& owners)
{
  std::vector<WatchNotice> notices;
  const ConversationSnapshot& snapshot = ended.snapshot;
  const bool english = lang == VoiceLang::En;
  Json::Value data(Json::objectValue);
  data["conversationId"] = snapshot.conversationId;
  data["cameraId"] = Json::Int64{snapshot.cameraId};
  data["cameraName"] = snapshot.cameraName;
  if (snapshot.userId > 0 && ended.reason == ConversationEnd::BusyLine) {
    Json::Value busy = data;
    busy["kind"] = "watch_busy";
    notices.push_back({.userId = snapshot.userId,
                       .type = "watch_busy",
                       .title = english ? "Argus heard you" : "Argus te escuchó",
                       .body = busyNotice(lang, snapshot.cameraName),
                       .data = busy});
  }
  if (snapshot.userId > 0 && snapshot.state == ConversationState::Resumable &&
      ended.reason != ConversationEnd::BusyLine) {
    Json::Value resume = data;
    resume["kind"] = "watch_resume";
    resume["resumableUntil"] = Json::Int64{snapshot.resumableUntil};
    notices.push_back({.userId = snapshot.userId,
                       .type = "watch_resume",
                       .title = english ? "Continue here" : "Continuar aquí",
                       .body = english ? "Your conversation with Argus can continue on this phone."
                                       : "Tu conversación con Argus puede seguir en este celular.",
                       .data = resume});
  }
  if (snapshot.userId > 0 && snapshot.audience == ConversationAudience::Discreet) {
    const std::string said = userWords(snapshot, "argus");
    if (!said.empty()) {
      Json::Value details = data;
      details["kind"] = "watch_details";
      notices.push_back({.userId = snapshot.userId,
                         .type = "watch_details",
                         .title = english ? "The details, as promised" : "Los detalles que te prometí",
                         .body = said,
                         .data = details});
    }
  }
  if (snapshot.audience == ConversationAudience::Visitor) {
    const std::string words = userWords(snapshot, "user");
    if (!words.empty()) {
      for (const int64_t owner : owners) {
        Json::Value message = data;
        message["kind"] = "watch_message";
        notices.push_back({.userId = owner,
                           .type = "watch_message",
                           .title = english ? "A visitor talked to Argus at " + snapshot.cameraName
                                            : "Un visitante habló con Argus en " + snapshot.cameraName,
                           .body = words,
                           .data = message});
      }
    }
  }
  return notices;
}

void WatchPublisher::update(const ConversationSnapshot& snapshot) const
{
  if (!input_.sync || snapshot.userId <= 0)
    return;
  SocketEmitDto frame;
  frame.operation = SyncOperation::WatchUpdate;
  frame.option = TableName::Notification;
  frame.obj = watchUpdateInfo(snapshot);
  if (!input_.sync->emitToUser(snapshot.userId, frame))
    LOG_WARN << "Voice watch: argus-sync refused the watch update of " << snapshot.conversationId;
}

std::vector<int64_t> WatchPublisher::owners() const
{
  std::vector<int64_t> out;
  if (!input_.identity)
    return out;
  const auto users = input_.identity->listUsers();
  if (!users)
    return out;
  for (const auto& user : *users)
    if (user.has_role() && parseUserRole(user.role()) == UserRole::Owner && (!user.has_is_active() || user.is_active()))
      out.push_back(user.user_id());
  return out;
}

void WatchPublisher::ended(const ConversationEnded& ended) const
{
  const bool visitor = ended.snapshot.audience == ConversationAudience::Visitor;
  for (const WatchNotice& notice : watchNoticesFor(ended, input_.lang, visitor ? owners() : std::vector<int64_t>{}))
    notify(notice);
}

void WatchPublisher::notify(const WatchNotice& notice) const
{
  if (!input_.notifications || notice.userId <= 0)
    return;
  argus::notification::v1::CreateNotificationsRequest request;
  request.add_user_ids(notice.userId);
  request.set_type(notice.type);
  request.set_title(notice.title);
  request.set_body(notice.body);
  request.set_data(compact(notice.data));
  request.set_command_id(commandIdOf(notice.type, notice.data.get("conversationId", "").asString(), notice.userId));
  const auto result =
      input_.notifications->createNotifications(request, {.userId = 0, .role = "system", .device = std::string(kDevice)});
  if (result.outcome != NotificationRpcOutcome::Success)
    LOG_WARN << "Voice watch: the " << notice.type << " notice was not created: " << result.status.error_message();
}
