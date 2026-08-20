#pragma once

#include <cstdint>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <string>
#include <vector>

struct WorkingMemory
{
  std::string lang;
  std::vector<ChatMessage> history;
  std::vector<int64_t> activeEntities;
  int64_t addresseeEntityId = 0;
};

struct TurnResult
{
  std::string reply;
  CaptureOutcome capture = CaptureOutcome::Rejected;
};

struct ConversationTurnInput
{
  int64_t userId = 0;
};

// Injected into the user turn when a capture fired so the assistant
// acknowledges it naturally. trimHistory strips it with the recall block.
// Deferred is NOT stored: formation still gets to reject it, so the wording
// must not promise a saved fact.
inline std::string captureAckNote(CaptureOutcome outcome,
                                  const std::string& lang)
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

  TurnResult processTurn(WorkingMemory& wm, const std::string& userText,
                         const ConversationTurnInput& input);

  // History ring with compaction enqueue (voice_test.history_messages cap).
  void trimHistory(WorkingMemory& wm, int64_t userId);

private:
  MemoryService& memory_;
  LlmService& llm_;
};
