#pragma once

#include <shared/utils/text-norm/text-norm.hxx>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The fast tier of the intent router: what the turn is for, decided in
// microseconds before the LLM is asked to write anything.
namespace intent
{

enum class ToolIntent : uint8_t
{
  None = 0,
  MemorySave,
  MemoryRecall,
  ReminderSet,
  MemoryForget,
  Camera,
  Unknown
};

// The training labels, in the enum's own order; the classifier reads them
// back and the accuracy gate compares fixtures against them.
inline constexpr std::array<std::pair<std::string_view, ToolIntent>, 6> kIntentNames{{
    {"none", ToolIntent::None},
    {"memory_save", ToolIntent::MemorySave},
    {"memory_recall", ToolIntent::MemoryRecall},
    {"reminder_set", ToolIntent::ReminderSet},
    {"memory_forget", ToolIntent::MemoryForget},
    {"camera", ToolIntent::Camera},
}};

constexpr std::string_view toolIntentToString(ToolIntent intent)
{
  for (const auto& [name, value] : kIntentNames) {
    if (value == intent)
      return name;
  }
  return "unknown";
}

constexpr ToolIntent toolIntentFromString(std::string_view name)
{
  for (const auto& [candidate, value] : kIntentNames) {
    if (candidate == name)
      return value;
  }
  return ToolIntent::Unknown;
}

struct IntentHit
{
  ToolIntent intent = ToolIntent::None;
  float score = 0.0F;
};

// The router's verdict for one turn. Unknown and not confident means the fast
// tier abstains, and the LLM's tool calling keeps the turn untouched.
struct IntentDecision
{
  ToolIntent intent = ToolIntent::Unknown;
  float score = 0.0F;
  float margin = 0.0F;
  bool fromRules = false;
  bool confident = false;
};

// The normalisation the published model was trained on: inverted marks
// stripped, punctuation to spaces, accents folded, lowercased, whitespace
// collapsed. Classifier and callers must agree on it or the scores drift.
inline std::string normalizeInput(const std::string& text)
{
  return text_norm::whitespace(
      text_norm::stripAccents(text_norm::intent(text)), true);
}

// Scores the already-normalised text, top-k so the router can measure a
// margin. A classifier that is not loaded never decides anything.
class IIntentClassifier
{
public:
  IIntentClassifier() = default;
  virtual ~IIntentClassifier() = default;

  IIntentClassifier(const IIntentClassifier&) = delete;
  IIntentClassifier& operator=(const IIntentClassifier&) = delete;
  IIntentClassifier(IIntentClassifier&&) = delete;
  IIntentClassifier& operator=(IIntentClassifier&&) = delete;

  virtual bool isLoaded() const = 0;
  virtual std::vector<IntentHit> score(const std::string& normalized) const = 0;
};

} // namespace intent
