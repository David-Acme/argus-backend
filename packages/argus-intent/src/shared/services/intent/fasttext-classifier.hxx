#pragma once

#include <shared/services/intent/intent-contracts.hxx>

#include <memory>
#include <string>
#include <vector>

namespace fasttext
{
class FastText;
}

// fastText wrapper: load once, score per call. isLoaded() is false when the
// model file is absent or ill-formed, and there the wrapper's job ends — the
// router degrades and the LLM tier keeps running.
class FastTextClassifier final : public intent::IIntentClassifier
{
public:
  // Absence or a load failure leaves the classifier unloaded; never throws
  // out to the caller.
  explicit FastTextClassifier(const std::string& modelPath);
  ~FastTextClassifier() override;

  bool isLoaded() const override;
  std::vector<intent::IntentHit>
  score(const std::string& normalized) const override;

private:
  std::unique_ptr<fasttext::FastText> model_;
};
