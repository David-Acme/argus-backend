#include "intent-service.hxx"

#include <algorithm>
#include <drogon/drogon.h>
#include <fasttext.h>
#include <memory>
#include <shared/services/config-service/config-service.hxx>
#include <shared/utils/text-norm/text-norm.hxx>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr const char* kLabelPrefix = "__label__";
constexpr int kLabelPrefixLen = 9;

ToolIntent labelToIntent(const std::string& label)
{
  if (label.rfind(kLabelPrefix, 0) == 0)
    return toolIntentFromString(label.substr(kLabelPrefixLen));
  return ToolIntent::Unknown;
}

double thresholdFor(ToolIntent intent)
{
  switch (intent) {
    case ToolIntent::Camera:
      return ConfigService::getDouble("intent.camera_threshold");
    case ToolIntent::MemorySave:
      return ConfigService::getDouble("intent.memory_save_threshold");
    default:
      return 2.0;
  }
}

float marginFloor()
{
  const double configured = ConfigService::getDouble("intent.margin");
  return configured > 0.0 ? static_cast<float>(configured) : 0.0F;
}

} // namespace

std::string toolIntentToString(ToolIntent intent)
{
  switch (intent) {
    case ToolIntent::Camera:
      return "camera";
    case ToolIntent::MemorySave:
      return "memory_save";
    case ToolIntent::None:
      return "none";
    default:
      return "unknown";
  }
}

ToolIntent toolIntentFromString(const std::string& label)
{
  if (label == "camera")
    return ToolIntent::Camera;
  if (label == "memory_save")
    return ToolIntent::MemorySave;
  if (label == "none")
    return ToolIntent::None;
  return ToolIntent::Unknown;
}

IntentService::IntentService() = default;

IntentService::~IntentService()
{
  shutdown();
}

void IntentService::init()
{
  std::unique_lock lock(modelMutex_);
  if (loaded_)
    return;

  const std::string modelPath = ConfigService::getString("intent.model");
  if (modelPath.empty()) {
    LOG_WARN << "IntentService: no intent.model configured — "
             << "implicit intents disabled (literal keywords only)";
    return;
  }

  try {
    model_ = std::make_unique<fasttext::FastText>();
    model_->loadModel(modelPath);
    loaded_ = true;
    const auto dict = model_->getDictionary();
    LOG_INFO << "IntentService: loaded (" << modelPath << ", "
             << (dict ? dict->nlabels() : 0) << " intents)";
  }
  catch (const std::exception& e) {
    LOG_WARN << "IntentService: load failed (" << modelPath << "): " << e.what()
             << " — implicit intents disabled "
             << "(literal keywords only)";
    model_.reset();
  }
}

void IntentService::shutdown()
{
  std::unique_lock lock(modelMutex_);
  model_.reset();
  loaded_ = false;
}

bool IntentService::isLoaded() const
{
  std::shared_lock lock(modelMutex_);
  return loaded_;
}

std::string IntentService::normalize(const std::string& text)
{
  return text_norm::intent(text);
}

bool IntentService::isMatchable(const std::string& text)
{
  const std::string normalized = normalize(text);
  size_t tokens = 0;
  size_t chars = 0;
  bool inToken = false;
  for (size_t i = 0; i < normalized.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(normalized[i]);
    if ((c & 0xC0) == 0x80)
      continue;
    ++chars;
    if (c == ' ') {
      inToken = false;
    }
    else if (!inToken) {
      inToken = true;
      ++tokens;
    }
  }
  return tokens >= 2 && chars >= 6;
}

std::vector<IntentHit> IntentService::match(const std::string& text) const
{
  std::vector<IntentHit> hits;
  std::shared_lock lock(modelMutex_);
  if (!loaded_ || text.empty() || !isMatchable(text))
    return hits;

  std::vector<std::pair<fasttext::real, std::string>> predictions;
  std::istringstream stream(normalize(text));
  model_->predictLine(stream, predictions, -1, 0.0F);
  hits.reserve(predictions.size());
  for (const auto& [score, label] : predictions) {
    const ToolIntent intent = labelToIntent(label);
    if (intent == ToolIntent::Unknown)
      continue;
    hits.push_back({.intent = intent, .score = score});
  }
  std::sort(hits.begin(), hits.end(),
            [](const IntentHit& a, const IntentHit& b) {
              return a.score > b.score;
            });
  return hits;
}

float IntentService::score(const std::vector<IntentHit>& hits,
                           ToolIntent intent)
{
  for (const auto& hit : hits) {
    if (hit.intent == intent)
      return hit.score;
  }
  return 0.0F;
}

float IntentService::margin(const std::vector<IntentHit>& hits)
{
  if (hits.empty())
    return 0.0F;
  return hits.front().score - (hits.size() > 1 ? hits[1].score : 0.0F);
}

bool IntentService::fired(const std::vector<IntentHit>& hits, ToolIntent intent)
{
  if (hits.empty() || hits.front().intent != intent)
    return false;
  if (hits.front().score < thresholdFor(intent))
    return false;
  return margin(hits) >= marginFloor();
}
