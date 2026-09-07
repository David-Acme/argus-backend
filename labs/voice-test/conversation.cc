#include "conversation.hxx"

#include <shared/services/config-service/config-service.hxx>

namespace
{

std::string stripInjectedContext(std::string text)
{
  static constexpr std::string_view kNotes[] = {
      "(Nota: el usuario te pidió recordar esto",
      "(Note: the user asked you to remember this",
  };
  for (const auto note : kNotes) {
    for (size_t at = text.find(note); at != std::string::npos;
         at = text.find(note, at)) {
      const size_t lineStart = text.rfind('\n', at);
      const size_t lineEnd = text.find('\n', at);
      const size_t begin = lineStart == std::string::npos ? 0 : lineStart;
      const size_t end =
          lineEnd == std::string::npos ? text.size() : lineEnd + 1;
      text.erase(begin, end - begin);
      at = begin;
    }
  }

  static constexpr std::pair<std::string_view, std::string_view> kBlocks[] = {
      {"<memorias>", "</memorias>"},
      {"<memories>", "</memories>"},
  };
  for (const auto& [open, close] : kBlocks) {
    for (size_t start = text.find(open); start != std::string::npos;
         start = text.find(open, start)) {
      const size_t body = start + open.size();
      const size_t closeAt = text.find(close, body);
      if (closeAt == std::string::npos)
        break;
      size_t end = closeAt + close.size();
      size_t lineEnd = text.find('\n', end);
      if (lineEnd != std::string::npos)  // trailing "use these facts" line
        lineEnd = text.find('\n', lineEnd + 1);
      end = lineEnd == std::string::npos ? text.size() : lineEnd + 1;
      if (start >= 2 && text.compare(start - 2, 2, "\n\n") == 0)
        start -= 2;
      text.erase(start, end - start);
    }
  }
  return text;
}

} // namespace

std::string captureAckNote(CaptureOutcome outcome, const std::string& lang)
{
  if (outcome == CaptureOutcome::Stored) {
    return lang == "en"
               ? "\n(Note: the user asked you to remember this and it is "
                 "stored. Briefly confirm that you noted it.)"
               : "\n(Nota: el usuario te pidió recordar esto y quedó "
                 "guardado. Confirma brevemente que lo has apuntado.)";
  }
  if (outcome == CaptureOutcome::Deferred) {
    return lang == "en"
               ? "\n(Note: the user asked you to remember this and you are "
                 "taking note now. Say you are noting it, in the present, "
                 "and never that it is already saved.)"
               : "\n(Nota: el usuario te pidió recordar esto y lo estás "
                 "apuntando ahora. Dilo en presente, y nunca que ya quedó "
                 "guardado.)";
  }
  return {};
}

std::string ConversationService::recallBlock(const RecallBlockInput& input)
{
  auto& wm = input.wm;
  if (input.userId >= 0 && wm.addresseeEntityId == 0)
    wm.addresseeEntityId = memory_.resolveAddresseeEntity(wm.lang);

  const int topK = ConfigService::getInt("memory.recall_top_k");
  const auto recalled = memory_.graphRecall().recall(
      {.text = input.text,
       .lang = wm.lang,
       .scope = "user",
       .refId = input.userId,
       .maxHops = 1,
       .limit = topK > 0 ? topK : 4,
       .addresseeEntityId = wm.addresseeEntityId,
       .activeEntityIds = wm.activeEntities});
  if (!recalled.usedIds.empty())
    memory_.bumpHitCount(recalled.usedIds);
  if (!recalled.usedEpisodeIds.empty())
    memory_.bumpEpisodeHits(recalled.usedEpisodeIds);
  if (!recalled.resolvedEntityIds.empty())
    wm.activeEntities = recalled.resolvedEntityIds;
  return recalled.block;
}

void ConversationService::trimHistory(WorkingMemory& wm, int64_t userId)
{
  const int cap = ConfigService::getInt("labs.conversation.history_messages");
  const size_t limit = cap > 0 ? static_cast<size_t>(cap) : 41;
  if (wm.history.size() <= limit || userId < 0)
    return;

  std::string transcript;
  const size_t dropped = wm.history.size() - limit;
  for (size_t i = 1; i <= dropped; ++i) {
    const auto& msg = wm.history[i];
    transcript +=
        (msg.role == "user" ? "user: " : "assistant: ") + msg.content + "\n";
  }
  wm.history.erase(wm.history.begin() + 1,
                   wm.history.begin() + 1 +
                       static_cast<std::ptrdiff_t>(dropped));
  if (!transcript.empty())
    memory_.enqueueCompaction(userId, transcript, wm.lang);

  // The prune already broke the prefill prefix; strip stale injected
  // context from the surviving user messages.
  for (auto& msg : wm.history) {
    if (msg.role == "user")
      msg.content = stripInjectedContext(msg.content);
  }
}
