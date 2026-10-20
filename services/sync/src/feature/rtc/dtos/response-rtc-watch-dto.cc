#include "response-rtc-watch-dto.hxx"

Json::Value watchConversationJson(const argus::voice::v1::WatchConversation& conversation)
{
  Json::Value json(Json::objectValue);
  json["conversationId"] = conversation.conversation_id();
  json["cameraId"] = Json::Int64{conversation.camera_id()};
  json["cameraName"] = conversation.camera_name();
  json["state"] = conversation.state();
  json["handoff"] = conversation.handoff();
  json["audience"] = conversation.audience();
  json["startedAt"] = Json::Int64{conversation.started_at()};
  json["updatedAt"] = Json::Int64{conversation.updated_at()};
  json["resumableUntil"] = conversation.resumable_until() > 0
                               ? Json::Value(Json::Int64{conversation.resumable_until()})
                               : Json::Value();
  json["endReason"] = conversation.end_reason().empty() ? Json::Value() : Json::Value(conversation.end_reason());
  Json::Value turns(Json::arrayValue);
  for (const auto& turn : conversation.turns()) {
    Json::Value entry(Json::objectValue);
    entry["id"] = Json::Int64{turn.id()};
    entry["role"] = turn.role();
    entry["text"] = turn.text();
    entry["at"] = Json::Int64{turn.at()};
    entry["final"] = turn.final();
    turns.append(entry);
  }
  json["turns"] = std::move(turns);
  return json;
}

Json::Value ResponseRtcWatchDto::toJson() const
{
  Json::Value json(Json::objectValue);
  Json::Value list(Json::arrayValue);
  for (const auto& conversation : conversations)
    list.append(watchConversationJson(conversation));
  json["conversations"] = std::move(list);
  return json;
}

Json::Value ResponseRtcWatchEndDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["ended"] = ended;
  return json;
}

Json::Value ResponseRtcWatchReplyDto::toJson() const
{
  Json::Value json(Json::objectValue);
  json["state"] = said ? "said" : "queued";
  json["replyUntil"] = Json::Int64{replyUntil};
  return json;
}
