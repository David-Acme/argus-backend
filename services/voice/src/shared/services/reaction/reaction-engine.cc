#include "reaction-engine.hxx"

#include <memory>
#include <phrase/phrase-catalog.hxx>
#include <phrase/rule-parser.hxx>

#include <algorithm>

struct ReactionEngine::Impl
{
  PhraseCatalog phrases;
  RuleParser rules{phrases};
  bool loaded = false;
};

namespace
{

float intensityFor(ReactionKind kind)
{
  switch (kind) {
    case ReactionKind::Warm:
      return 0.7F;
    case ReactionKind::Confused:
      return 0.6F;
    case ReactionKind::Acknowledging:
      return 0.5F;
    case ReactionKind::Thinking:
      return 0.45F;
    default:
      return 0.0F;
  }
}

}

ReactionEngine::ReactionEngine() : impl_(std::make_unique<Impl>()) {}

ReactionEngine::~ReactionEngine() = default;

void ReactionEngine::init()
{
  impl_->phrases.build();
  impl_->loaded = impl_->phrases.phraseCount() > 0;
}

bool ReactionEngine::isLoaded() const
{
  return impl_->loaded;
}

Reaction ReactionEngine::react(const ReactionSignals& signals) const
{
  const auto make = [](ReactionKind kind, const char* because) {
    return Reaction{.kind = kind, .intensity = intensityFor(kind), .because = because};
  };

  if (signals.sttFailed || signals.text.empty())
    return make(ReactionKind::Confused, "stt_failed");

  const RuleParseInput parsed{.text = signals.text, .lang = signals.lang};
  if (impl_->rules.isCancellation(parsed))
    return make(ReactionKind::Acknowledging, "retraction");
  if (impl_->rules.isQuestion(parsed))
    return make(ReactionKind::Thinking, "question");
  if (impl_->rules.isVacuous(parsed))
    return make(ReactionKind::Warm, "small_talk");
  return make(ReactionKind::Idle, "no_signal");
}

std::string ReactionEngine::toneNote(const Reaction& reaction,
                                     const std::string& lang)
{
  const bool en = lang == "en";
  switch (reaction.kind) {
    case ReactionKind::Confused:
      return en ? "\n(Tone: you did not catch it. Ask them to repeat, once.)"
                : "\n(Tono: no lo has captado. Pide que lo repita, una vez.)";
    case ReactionKind::Warm:
      return en ? "\n(Tone: warm and short.)" : "\n(Tono: cálido y corto.)";
    default:
      return {};
  }
}
