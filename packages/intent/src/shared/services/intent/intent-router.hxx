#pragma once

#include <shared/services/intent/intent-contracts.hxx>

#include <functional>
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

  explicit IntentRouter(IntentRouterInput input);

  intent::IntentDecision decide(const std::string& text,
                                const std::string& lang) const;

private:
  intent::ToolIntent factOrReminder(const std::string& text,
                                    const std::string& lang) const;

  bool recallMarkerOpens(const std::string& lowered,
                         const std::string& lang) const;

  const PhraseCatalog& catalog_;
  const intent::IIntentClassifier& model_;
  intent::TemporalProbe recurrent_;
};
