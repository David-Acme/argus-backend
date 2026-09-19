#pragma once

#include <shared/services/intent/intent-contracts.hxx>

#include <functional>
#include <string>

class PhraseCatalog;

namespace intent
{

// Rubric probe: atemporal or recurring -> save, single future instant -> reminder.
using TemporalProbe =
    std::function<bool(const std::string& text, const std::string& lang)>;

} // namespace intent

struct IntentRouterInput
{
  const PhraseCatalog& catalog;
  const intent::IIntentClassifier& model;
  intent::TemporalProbe recurrent;
};

// Rules first, fastText second, a temporal probe last; unconfident hands the turn back.
class IntentRouter
{
public:
  // Operating point from the held-out judge corpus (models/intent/MODEL-CARD.md).
  static constexpr float kThreshold = 0.90F;
  static constexpr float kMargin = 0.10F;

  explicit IntentRouter(IntentRouterInput input);

  intent::IntentDecision decide(const std::string& text,
                                const std::string& lang) const;

private:
  // Recurring or absent time stays a fact; a single future instant is a reminder.
  intent::ToolIntent factOrReminder(const std::string& text,
                                    const std::string& lang) const;

  // A recall marker decides only when it opens the turn; mid-text is narration.
  bool recallMarkerOpens(const std::string& lowered,
                         const std::string& lang) const;

  const PhraseCatalog& catalog_;
  const intent::IIntentClassifier& model_;
  intent::TemporalProbe recurrent_;
};
