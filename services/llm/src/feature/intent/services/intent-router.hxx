#pragma once

#include <feature/intent/services/intent-contracts.hxx>

#include <functional>
#include <optional>
#include <string>

class PhraseCatalog;

namespace intent
{

using TemporalProbe =
    std::function<bool(const std::string& text, const std::string& lang)>;

}

struct IntentRouterInput
{
  const PhraseCatalog& catalog;
  const intent::IIntentClassifier& model;
  intent::TemporalProbe recurrent;
};

class IntentRouter
{
public:
  static constexpr float kThreshold = 0.90F;
  static constexpr float kMargin = 0.10F;
  static constexpr float kAgreeFloor = 0.50F;

  explicit IntentRouter(IntentRouterInput input);

  [[nodiscard]] intent::IntentDecision decide(const std::string& text,
                                              const std::string& lang) const;

private:
  struct RuleProposal
  {
    intent::ToolIntent intent;
    intent::DecisionSource source;
  };

  [[nodiscard]] std::optional<RuleProposal> propose(const std::string& text,
                                                    const std::string& lang) const;

  [[nodiscard]] bool asksToBeReminded(const std::string& text) const;

  [[nodiscard]] intent::ToolIntent factOrReminder(const std::string& text,
                                                  const std::string& lang) const;

  [[nodiscard]] bool recallMarkerOpens(const std::string& lowered,
                                       const std::string& lang) const;

  const PhraseCatalog& catalog_;
  const intent::IIntentClassifier& model_;
  intent::TemporalProbe recurrent_;
};
