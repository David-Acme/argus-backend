#pragma once

#include <memory>
#include <voice/reaction-contracts.hxx>
#include <string>

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

  static std::string toneNote(const Reaction& reaction,
                              const std::string& lang);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
