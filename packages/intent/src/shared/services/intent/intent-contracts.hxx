#pragma once

#include <shared/utils/text-norm/text-norm.hxx>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The fast tier of the intent router: what the turn is for, in microseconds.
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

// The training labels, in the enum's own order.
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

// Unknown and not confident means the fast tier abstains; the LLM keeps the turn.
struct IntentDecision
{
  ToolIntent intent = ToolIntent::Unknown;
  float score = 0.0F;
  float margin = 0.0F;
  bool fromRules = false;
  bool confident = false;
};

// The normalisation the published model was trained on; both sides must agree.
inline std::string normalizeInput(const std::string& text)
{
  return text_norm::whitespace(
      text_norm::stripAccents(text_norm::intent(text)), true);
}

// Scores already-normalised text, top-k so the router can measure a margin.
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
