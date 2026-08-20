#include "reaction-engine.hxx"

#include <algorithm>

namespace
{

float intensityFor(ReactionKind kind, const ReactionSignals& signals)
{
  switch (kind) {
    case ReactionKind::Alarmed:
      return 1.0F;
    case ReactionKind::Recognizing: {
      const int hits = std::clamp(signals.recallHits, 0, 3);
      return 0.5F + static_cast<float>(hits) * 0.15F;
    }
    case ReactionKind::Attentive:
      return signals.captureStored ? 0.8F : 0.6F;
    case ReactionKind::Curious:
      return 0.7F;
    case ReactionKind::Warm:
      return 0.7F;
    case ReactionKind::Confused:
      return 0.6F;
    case ReactionKind::Uncertain:
      return 0.6F;
    case ReactionKind::Acknowledging:
      return 0.5F;
    case ReactionKind::Thinking:
      return 0.45F;
    default:
      return 0.0F;
  }
}

} // namespace

void ReactionEngine::init()
{
  phrases_.build();
  loaded_ = phrases_.phraseCount() > 0;
}

Reaction ReactionEngine::react(const ReactionSignals& signals) const
{
  const auto make = [&](ReactionKind kind, const char* because) {
    return Reaction{.kind = kind,
                    .intensity = intensityFor(kind, signals),
                    .because = because};
  };

  // Resolution order IS the priority: an alarm must never be buried under
  // "thinking", and the first match wins.
  if (signals.systemAlert)
    return make(ReactionKind::Alarmed, "system_alert");
  if (signals.sttFailed || signals.text.empty())
    return make(ReactionKind::Confused, "stt_failed");

  const RuleParseInput parsed{.text = signals.text, .lang = signals.lang};
  if (rules_.isCancellation(parsed))
    return make(ReactionKind::Acknowledging, "retraction");
  if (signals.recallHits > 0)
    return make(ReactionKind::Recognizing, "recall_hit");
  if (signals.captureStored)
    return make(ReactionKind::Attentive, "capture_stored");
  if (signals.captureQueued)
    return make(ReactionKind::Attentive, "capture_queued");
  if (signals.cameraIntent)
    return make(ReactionKind::Curious, "camera_intent");

  const bool question = rules_.isQuestion(parsed);
  if (question && signals.recallHits == 0)
    return make(ReactionKind::Uncertain, "recall_empty");
  if (question)
    return make(ReactionKind::Thinking, "question");
  if (rules_.isVacuous(parsed))
    return make(ReactionKind::Warm, "small_talk");
  return make(ReactionKind::Idle, "no_signal");
}

std::string ReactionEngine::toneNote(const Reaction& reaction,
                                     const std::string& lang)
{
  const bool en = lang == "en";
  switch (reaction.kind) {
    case ReactionKind::Alarmed:
      return en ? "\n(Tone: this is urgent. Be brief and concrete.)"
                : "\n(Tono: esto es urgente. Sé breve y concreto.)";
    case ReactionKind::Confused:
      return en ? "\n(Tone: you did not catch it. Ask them to repeat, once.)"
                : "\n(Tono: no lo has captado. Pide que lo repita, una vez.)";
    case ReactionKind::Uncertain:
      return en ? "\n(Tone: you do not have this. Say so plainly, no guessing.)"
                : "\n(Tono: esto no lo tienes. Dilo con claridad, sin "
                  "adivinar.)";
    case ReactionKind::Recognizing:
      return en ? "\n(Tone: you already knew this. Answer like someone who "
                  "remembers, without announcing it.)"
                : "\n(Tono: esto ya lo sabías. Responde como quien lo "
                  "recuerda, sin anunciarlo.)";
    case ReactionKind::Warm:
      return en ? "\n(Tone: warm and short.)" : "\n(Tono: cálido y corto.)";
    default:
      return {};
  }
}
