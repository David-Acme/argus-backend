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

// Injected into the user turn when a capture fired so the assistant
// acknowledges it naturally. trimHistory strips it with the recall block.
// Deferred is NOT stored: formation still gets to reject it, so the wording
// must not promise a saved fact.
std::string captureAckNote(CaptureOutcome outcome, const std::string& lang);

class ConversationService
{
public:
  ConversationService(MemoryService& memory) : memory_(memory) {}

  // Entity-anchored recall block for the turn text. Updates wm.activeEntities
  // with the entities resolved this turn (anaphora source for follow-ups) and
  // bumps hit counts.
  std::string recallBlock(WorkingMemory& wm, const std::string& text,
                          int64_t userId);

  // History ring with compaction enqueue.
  void trimHistory(WorkingMemory& wm, int64_t userId);

private:
  MemoryService& memory_;
};
