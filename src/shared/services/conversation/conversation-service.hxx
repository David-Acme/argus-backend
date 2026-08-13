#pragma once

#include <cstdint>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <shared/services/tools/tool-executor.hxx>
#include <string>
#include <vector>

// Turn state machine (COGNITIVE_MEMORY_PLAN.md §7):
// LISTEN -> UNDERSTAND -> RETRIEVE -> DECIDE -> (RESPOND/FORM | ACT -> OBSERVE)
// Replaces the three duplicated loops in labs/voice-test. WorkingMemory
// (history ring, active entities, last tool results, language) is owned by
// the caller (the conversation session).
struct WorkingMemory
{
  std::string lang;
  std::vector<ChatMessage> history;
  std::vector<int64_t> activeEntities;
  std::vector<tools::ToolResult> lastToolResults;
  int64_t addresseeEntityId = 0;
};

struct TurnResult
{
  std::string reply;
  bool acted = false;
  std::vector<tools::ToolCall> toolCalls;
};

// Injected into the user turn when a capture fired so the assistant
// acknowledges it naturally. trimHistory strips it with the recall block.
inline std::string captureAckNote(const std::string& lang)
{
  return lang == "en"
             ? "\n(Note: the user asked you to remember this. Briefly confirm "
               "that you noted it.)"
             : "\n(Nota: el usuario te pidió recordar esto. Confirma "
               "brevemente que lo has apuntado.)";
}

class ConversationService
{
public:
  ConversationService(MemoryService& memory, LlmService& llm)
      : memory_(memory), llm_(llm)
  {
  }

  // RETRIEVE: entity-anchored recall block for the turn text. Updates
  // wm.activeEntities with the entities resolved this turn (anaphora source
  // for follow-ups) and bumps hit counts.
  std::string recallBlock(WorkingMemory& wm, const std::string& text,
                          int64_t userId);

  // DECIDE + ACT + OBSERVE + RESPOND: chat with the registered tools
  // (bounded tool loop), then FORM (deterministic capture of the turn).
  TurnResult processTurn(WorkingMemory& wm, const std::string& userText,
                         int64_t userId, UserRole role, int maxToolHops = 3);

  // History ring with compaction enqueue (voice_test.history_messages cap).
  void trimHistory(WorkingMemory& wm, int64_t userId);

private:
  MemoryService& memory_;
  LlmService& llm_;
};
