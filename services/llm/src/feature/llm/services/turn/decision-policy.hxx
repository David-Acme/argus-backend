#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace turn
{

struct DecisionThresholds
{
  double act{0.90};
  double ask{0.90};
  double margin{0.0};
};

using DecisionToolClasses = std::vector<std::pair<std::vector<std::string>, DecisionThresholds>>;

[[nodiscard]] inline bool classNamesTool(const DecisionToolClasses& classes, std::string_view tool)
{
  return std::ranges::any_of(classes, [tool](const auto& entry) {
    return std::ranges::find(entry.first, tool) != entry.first.end();
  });
}

[[nodiscard]] inline DecisionThresholds thresholdsFor(const DecisionToolClasses& classes,
                                                     const DecisionThresholds& fallback, std::string_view tool)
{
  const auto found = std::ranges::find_if(classes, [tool](const auto& entry) {
    return std::ranges::find(entry.first, tool) != entry.first.end();
  });
  if (found == classes.end())
    return fallback;
  const DecisionThresholds& declared = found->second;
  return {.act = std::max(declared.act, fallback.act),
          .ask = std::max(declared.ask, fallback.ask),
          .margin = std::max(declared.margin, fallback.margin)};
}

struct DecisionPolicy
{
  double act{0.90};
  double ask{0.90};
  double margin{0.0};
  double nowMin{0.0};
  bool witnessOnly{false};
  DecisionToolClasses toolClasses{};

  [[nodiscard]] DecisionThresholds thresholdsFor(std::string_view tool) const
  {
    return turn::thresholdsFor(toolClasses, DecisionThresholds{.act = act, .ask = ask, .margin = margin}, tool);
  }
};

enum class Verdict : unsigned char
{
  Act,
  Ask,
  Choose,
  Pass
};

struct Reading
{
  double confidence{0.0};
  std::optional<double> runnerUp;
};

[[nodiscard]] constexpr Verdict judge(const DecisionThresholds& thresholds, const Reading& reading)
{
  if (reading.confidence < thresholds.ask)
    return Verdict::Pass;
  if (reading.runnerUp && *reading.runnerUp >= thresholds.ask && reading.confidence - *reading.runnerUp < thresholds.margin)
    return Verdict::Choose;
  return reading.confidence >= thresholds.act ? Verdict::Act : Verdict::Ask;
}

class PolicySet
{
public:
  PolicySet() = default;
  explicit PolicySet(const DecisionPolicy& fallback) : fallback_(fallback) {}

  void set(std::string id, const DecisionPolicy& policy) { byDecider_[std::move(id)] = policy; }

  void setFallback(const DecisionPolicy& policy) { fallback_ = policy; }

  [[nodiscard]] const DecisionPolicy& of(std::string_view id) const
  {
    const auto found = byDecider_.find(id);
    return found == byDecider_.end() ? fallback_ : found->second;
  }

private:
  DecisionPolicy fallback_;
  std::map<std::string, DecisionPolicy, std::less<>> byDecider_;
};

}
