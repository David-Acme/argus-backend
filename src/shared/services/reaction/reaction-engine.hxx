#pragma once

#include <memory>
#include <shared/services/reaction/reaction-contracts.hxx>
#include <string>

// Turns the signals a turn already produced into the assistant's reaction.
// Deliberately NOT a model: every decision is a rule over signals the
// pipeline computed anyway, so a wrong face is explainable from `because`.
// The rule machinery lives behind the pimpl so the voice path compiles
// without memory types (Ruling CA).
class ReactionEngine
{
public:
  ReactionEngine();
  ~ReactionEngine();

  ReactionEngine(const ReactionEngine&) = delete;
  ReactionEngine& operator=(const ReactionEngine&) = delete;

  void init();
  bool isLoaded() const;

  Reaction react(const ReactionSignals& signals) const;

  // Short line appended to the TAIL of the user turn, never to the system
  // prompt: the system prefix has to stay constant for KV-cache reuse.
  static std::string toneNote(const Reaction& reaction,
                              const std::string& lang);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
