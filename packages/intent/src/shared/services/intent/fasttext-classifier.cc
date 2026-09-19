#include "fasttext-classifier.hxx"

#include <fasttext.h>

#include <exception>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace
{

constexpr std::string_view kLabelPrefix = "__label__";

// fastText hands back the raw training label, "__label__memory_save".
intent::ToolIntent intentFromLabel(std::string_view label)
{
  if (label.starts_with(kLabelPrefix))
    label.remove_prefix(kLabelPrefix.size());
  return intent::toolIntentFromString(label);
}

} // namespace

FastTextClassifier::FastTextClassifier(const std::string& modelPath)
{
  const std::ifstream probe(modelPath, std::ifstream::binary);
  if (!probe.is_open())
    return;
  auto model = std::make_unique<fasttext::FastText>();
  try {
    model->loadModel(modelPath);
    model_ = std::move(model);
  }
  catch (const std::exception&) {
  }
}

FastTextClassifier::~FastTextClassifier() = default;

bool FastTextClassifier::isLoaded() const
{
  return model_ != nullptr;
}

// Appends the trailing newline Python's predict() does; without it scores drift.
std::vector<intent::IntentHit>
FastTextClassifier::score(const std::string& normalized) const
{
  if (!model_)
    return {};
  std::istringstream in(normalized + "\n");
  std::vector<std::pair<fasttext::real, std::string>> predictions;
  model_->predictLine(in, predictions, 2, 0.0F);

  std::vector<intent::IntentHit> hits;
  hits.reserve(predictions.size());
  for (const auto& [probability, label] : predictions)
    hits.push_back({.intent = intentFromLabel(label), .score = probability});
  return hits;
}
