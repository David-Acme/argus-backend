#pragma once

#include <feature/intent/services/intent-contracts.hxx>

#include <memory>
#include <string>
#include <vector>

namespace fasttext
{
class FastText;
}

class FastTextClassifier final : public intent::IIntentClassifier
{
public:
  explicit FastTextClassifier(const std::string& modelPath);
  ~FastTextClassifier() override;

  bool isLoaded() const override;
  std::vector<intent::IntentHit>
  score(const std::string& normalized) const override;

private:
  std::unique_ptr<fasttext::FastText> model_;
};
