#pragma once

#include <text/text-norm.hxx>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

enum class DecisionSource : uint8_t
{
  Model = 0,
  Trigger,
  Cancellation,
  Statement,
  RecallMarker
};

struct IntentHit
{
  ToolIntent intent = ToolIntent::None;
  float score = 0.0F;
};

struct IntentDecision
{
  ToolIntent intent = ToolIntent::Unknown;
  float score = 0.0F;
  float margin = 0.0F;
  bool fromRules = false;
  bool confident = false;
  DecisionSource source = DecisionSource::Model;
  ToolIntent runnerUp = ToolIntent::None;
  float runnerUpScore = 0.0F;
};

inline std::string normalizeInput(const std::string& text)
{
  return text_norm::whitespace(
      text_norm::stripAccents(text_norm::intent(text)), true);
}

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

}
