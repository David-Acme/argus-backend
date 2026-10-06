#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace turn
{

struct DecisionPolicy
{
  double act{0.90};
  double ask{0.90};
  double margin{0.0};
  bool witnessOnly{false};
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

[[nodiscard]] constexpr Verdict judge(const DecisionPolicy& policy, const Reading& reading)
{
  if (reading.confidence < policy.ask)
    return Verdict::Pass;
  if (reading.runnerUp && *reading.runnerUp >= policy.ask && reading.confidence - *reading.runnerUp < policy.margin)
    return Verdict::Choose;
  return reading.confidence >= policy.act ? Verdict::Act : Verdict::Ask;
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
