#pragma once

#include <memory>
#include <shared/services/reaction/reaction-contracts.hxx>
#include <string>

// Turns the signals a turn already produced into the assistant's reaction; every decision is a rule.
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

  // Short line appended to the tail of the user turn, never to the system prompt.
  static std::string toneNote(const Reaction& reaction,
                              const std::string& lang);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
