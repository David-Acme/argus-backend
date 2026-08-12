#include "conversation-service.hxx"

#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/llm/lfm-adapter.hxx>
#include <shared/services/tools/tool-registry.hxx>

std::string ConversationService::recallBlock(const std::string& text,
                                             int64_t userId,
                                             const std::string& lang)
{
  const auto recalled = memory_.graphRecall().recall({.text = text,
                                                      .lang = lang,
                                                      .scope = "user",
                                                      .refId = userId,
                                                      .maxHops = 1,
                                                      .limit = 8,
                                                      .addresseeEntityId = 0});
  if (!recalled.usedIds.empty())
    memory_.bumpHitCount(recalled.usedIds);
  return recalled.block;
}

TurnResult ConversationService::processTurn(WorkingMemory& wm,
                                            const std::string& userText,
                                            int64_t userId, UserRole role,
                                            int maxToolHops)
{
  TurnResult result;
  if (userText.empty())
    return result;

  // LISTEN -> UNDERSTAND: capture deterministically first ("recuerda que X"
  // must never wait for the model).
  memory_.captureExplicit(
      {.userId = userId, .lang = wm.lang, .text = userText});

  // RETRIEVE: entity-anchored block injected into the turn.
  const std::string block = recallBlock(userText, userId, wm.lang);

  // DECIDE + ACT + RESPOND: chat with the registered tools.
  ToolRegistry& registry = ToolRegistry::instance();
  ToolExecutor executor(registry);
  const auto names = registry.names();
  std::vector<const tools::ToolDescriptor*> tools;
  tools.reserve(names.size());
  for (const auto& name : names) {
    if (const auto* descriptor = registry.find(name))
      tools.push_back(descriptor);
  }

  LfmAdapter adapter(llm_);
  // The system prompt stays constant so the prefill prefix is reusable, and
  // the recalled facts ride at the tail of the user turn: measured on the
  // 1.2B, facts placed before the question got answered from the previous
  // turn's entity ("¿qué no le gusta a Pedro?" -> "tu hermana...").
  const std::string system =
      "Eres Argus, el asistente del hogar. Responde brevemente en el idioma "
      "del usuario.";

  if (wm.history.empty())
    wm.history.push_back({.role = "system", .content = system});
  else
    wm.history.front().content = system;
  wm.history.push_back(
      {.role = "user",
       .content = block.empty() ? userText : userText + "\n\n" + block});

  const auto output = adapter.chatWithTools({.systemPrompt = system,
                                             .tools = tools,
                                             .role = role,
                                             .context = {.userId = userId,
                                                         .lang = wm.lang,
                                                         .sessionId = {}},
                                             .maxHops = maxToolHops},
                                            wm.history);
  result.reply = output.reply;
  result.toolCalls = output.executed;
  result.acted = !output.executed.empty();

  std::string chain;
  for (const auto& call : output.executed)
    chain += call.name + "\n";
  if (result.acted && userId >= 0)
    memory_.recordProcedure("chain:" + std::to_string(output.executed.size()),
                            userText, chain);

  trimHistory(wm, userId);
  return result;
}

void ConversationService::trimHistory(WorkingMemory& wm, int64_t userId)
{
  const int cap = ConfigService::getInt("voice_test.history_messages");
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
}
