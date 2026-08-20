#pragma once

#include <shared/services/memory/phrase-catalog.hxx>
#include <shared/services/memory/rule-parser.hxx>
#include <shared/services/reaction/reaction-contracts.hxx>
#include <string>

// Turns the signals a turn already produced into the assistant's reaction.
// Deliberately NOT a model: every decision is a rule over signals the
// pipeline computed anyway, so a wrong face is explainable from `because`.
// Self-contained (own catalogue, ~137 KB) so the lab and the WebSocket path
// can both use it without dragging MemoryService in.
class ReactionEngine
{
public:
  void init();
  bool isLoaded() const { return loaded_; }

  Reaction react(const ReactionSignals& signals) const;

  // Short line appended to the TAIL of the user turn, never to the system
  // prompt: the system prefix has to stay constant for KV-cache reuse.
  static std::string toneNote(const Reaction& reaction,
                              const std::string& lang);

private:
  PhraseCatalog phrases_;
  RuleParser rules_{phrases_};
  bool loaded_ = false;
};
