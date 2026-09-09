#pragma once

#include <shared/services/intent/intent-contracts.hxx>

#include <functional>
#include <string>

class PhraseCatalog;

namespace intent
{

// True when the text is atemporal or its time recurs (rubric: atemporal and
// recurring -> memory_save, a single future instant -> reminder_set).
// Injected so this package never links the memory graph.
using TemporalProbe =
    std::function<bool(const std::string& text, const std::string& lang)>;

} // namespace intent

struct IntentRouterInput
{
  const PhraseCatalog& catalog;
  const intent::IIntentClassifier& model;
  intent::TemporalProbe recurrent;
};

// Rules first (explicit triggers, microseconds), fastText second (threshold
// plus margin over the runner-up), a temporal probe for the one ambiguity the
// taxonomy leaves open. An unconfident or unloaded decision hands the turn
// back to the LLM's tool calling, byte for byte.
class IntentRouter
{
public:
  // Measured on the held-out judge corpus (models/intent/MODEL-CARD.md): the
  // precision floors are met at this operating point.
  static constexpr float kThreshold = 0.90F;
  static constexpr float kMargin = 0.10F;

  explicit IntentRouter(IntentRouterInput input);

  intent::IntentDecision decide(const std::string& text,
                                const std::string& lang) const;

private:
  // The taxonomy's one ambiguity under an explicit trigger: a recurring or
  // absent time stays a fact, a single future instant is a reminder.
  intent::ToolIntent factOrReminder(const std::string& text,
                                    const std::string& lang) const;

  // A recall marker decides the class only when it opens the turn; mid-text
  // markers ("me acuerdo de cuando") are past-tense narration, not a query.
  bool recallMarkerOpens(const std::string& lowered,
                         const std::string& lang) const;

  const PhraseCatalog& catalog_;
  const intent::IIntentClassifier& model_;
  intent::TemporalProbe recurrent_;
};
