#pragma once

#include <shared/services/intent/intent-contracts.hxx>

#include <memory>
#include <string>
#include <vector>

namespace fasttext
{
class FastText;
}

// fastText wrapper: load once, score per call; unloaded leaves routing to the LLM tier.
class FastTextClassifier final : public intent::IIntentClassifier
{
public:
  // Absence or a load failure leaves the classifier unloaded; never throws.
  explicit FastTextClassifier(const std::string& modelPath);
  ~FastTextClassifier() override;

  bool isLoaded() const override;
  std::vector<intent::IntentHit>
  score(const std::string& normalized) const override;

private:
  std::unique_ptr<fasttext::FastText> model_;
};
