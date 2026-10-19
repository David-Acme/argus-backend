#include "watch-directory.hxx"

argus::voice::v1::WatchConversation watchConversationOf(const ConversationSnapshot& snapshot)
{
  argus::voice::v1::WatchConversation out;
  out.set_conversation_id(snapshot.conversationId);
  out.set_user_id(snapshot.userId);
  out.set_camera_id(snapshot.cameraId);
  out.set_camera_name(snapshot.cameraName);
  out.set_state(std::string(conversationStateToString(snapshot.state)));
  out.set_handoff(std::string(conversationHandoffToString(snapshot.handoff)));
  out.set_audience(std::string(conversationAudienceToString(snapshot.audience)));
  out.set_started_at(snapshot.startedAt);
  out.set_updated_at(snapshot.updatedAt);
  out.set_resumable_until(snapshot.resumableUntil);
  out.set_end_reason(std::string(conversationEndToString(snapshot.endReason)));
  for (const ConversationTurn& turn : snapshot.turns) {
    auto* entry = out.add_turns();
    entry->set_id(turn.id);
    entry->set_role(turn.role);
    entry->set_text(turn.text);
    entry->set_at(turn.at);
    entry->set_final(turn.final);
  }
  return out;
}

std::vector<argus::voice::v1::WatchConversation> ConversationDirectory::conversations(int64_t userId) const
{
  std::vector<argus::voice::v1::WatchConversation> out;
  if (!manager_)
    return out;
  for (const ConversationSnapshot& snapshot : manager_->list(userId))
    out.push_back(watchConversationOf(snapshot));
  return out;
}

bool ConversationDirectory::end(int64_t userId, const std::string& conversationId)
{
  return manager_ && manager_->end(userId, conversationId);
}
