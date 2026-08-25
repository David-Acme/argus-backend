#include "conversation-service.hxx"

#include <shared/services/config-service/config-service.hxx>

namespace
{

// Strips injected context (ack notes, <memorias>/<memories> blocks) from a
// user message. Only run when the history head is already being pruned.
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

std::string ConversationService::recallBlock(WorkingMemory& wm,
                                             const std::string& text,
                                             int64_t userId)
{
  if (userId >= 0 && wm.addresseeEntityId == 0)
    wm.addresseeEntityId = memory_.resolveAddresseeEntity(wm.lang);

  const int topK = ConfigService::getInt("memory.recall_top_k");
  const auto recalled = memory_.graphRecall().recall(
      {.text = text,
       .lang = wm.lang,
       .scope = "user",
       .refId = userId,
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

TurnResult ConversationService::processTurn(WorkingMemory& wm,
                                            const std::string& userText,
                                            const ConversationTurnInput& input)
{
  TurnResult result;
  if (userText.empty())
    return result;

  // LISTEN -> UNDERSTAND: capture deterministically first ("recuerda que X"
  // must never wait for the model).
  const CaptureResult capture = memory_.captureExplicit(
      {.userId = input.userId, .lang = wm.lang, .text = userText});
  result.capture = capture.outcome;

  // RETRIEVE: entity-anchored block injected into the turn.
  const std::string block = recallBlock(wm, userText, input.userId);

  // The system prompt stays constant so the prefill prefix is reusable, and
  // the recalled facts ride at the tail of the user turn: measured on the
  // 1.2B, facts placed before the question got answered from the previous
  // turn's entity ("¿qué no le gusta a Pedro?" -> "tu hermana...").
  std::string system =
      wm.lang == "en"
          ? "You are Argus, the home assistant. Reply briefly in the user's "
            "language, like a person, and never with generic offers. "
            "Questions, sums, dates, times and definitions are ephemeral "
            "conversation, not memory. Memory is managed outside you: never "
            "emit tool calls, JSON or save blocks, and only confirm that "
            "something was stored when the turn carries a capture note."
          : "Eres Argus, el asistente del hogar. Responde brevemente en el "
            "idioma del usuario, como una persona, y nunca con ofertas "
            "genéricas. Las preguntas, cálculos, fechas, horas y definiciones "
            "son conversación efímera, no memoria. La memoria se administra "
            "fuera de ti: no emitas tool calls, JSON ni bloques de guardado, "
            "y confirma que algo quedó guardado solo si el turno trae una "
            "nota de captura.";
  if (input.userId >= 0) {
    const std::string profile = memory_.profileFor(input.userId, wm.lang);
    if (!profile.empty())
      system += "\n\n" + profile;
  }

  if (wm.history.empty())
    wm.history.push_back({.role = "system", .content = system});
  else
    wm.history.front().content = system;

  std::string content = userText;
  content += captureAckNote(capture.outcome, wm.lang);
  if (!block.empty())
    content += "\n\n" + block;
  wm.history.push_back({.role = "user", .content = content});

  const ChatRequest req{.messages = wm.history,
                        .maxTokens = 0,
                        .temperature = -1.0F,
                        .resetContext = false};
  result.reply = llm_.chat(req);
  wm.history.push_back({.role = "assistant", .content = result.reply});

  trimHistory(wm, input.userId);
  return result;
}

void ConversationService::trimHistory(WorkingMemory& wm, int64_t userId)
{
  const int cap = ConfigService::getInt("conversation.history_messages");
  const size_t limit = cap > 0 ? static_cast<size_t>(cap) : 21;
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
