#pragma once

#include <auth/module-snapshot.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <json/value.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace turn
{

inline constexpr std::array<std::string_view, 2> kDeciderIds{"rules", "router"};

struct Pick
{
  std::string tool;
  Json::Value arguments{Json::objectValue};
  std::vector<std::string> fill;
  double confidence{0.0};
};

struct Candidate
{
  std::string tool;
  Json::Value arguments{Json::objectValue};
  std::vector<std::string> fill;
  double confidence{1.0};
  std::string source;
  std::string decider;
  bool exact{false};
  bool confident{true};
  std::optional<Pick> runnerUp;
};

struct DecideInput
{
  std::string_view utterance;
  std::string_view lang;
  const std::vector<tools::ToolHandle>& offered;
  const ModuleSnapshot& modules;
};

class Decider
{
public:
  Decider() = default;
  virtual ~Decider() = default;
  Decider(const Decider&) = delete;
  Decider& operator=(const Decider&) = delete;

  [[nodiscard]] virtual std::string_view id() const = 0;

  [[nodiscard]] virtual std::optional<Candidate> decide(const DecideInput& input) const = 0;
};

[[nodiscard]] inline bool isOffered(const DecideInput& input, std::string_view tool)
{
  for (const auto& handle : input.offered)
    if (handle->spec.name == tool)
      return true;
  return false;
}

[[nodiscard]] inline Candidate candidateOf(const Pick& pick, const Candidate& from)
{
  return {.tool = pick.tool,
          .arguments = pick.arguments,
          .fill = pick.fill,
          .confidence = pick.confidence,
          .source = from.source,
          .decider = from.decider,
          .exact = false,
          .confident = true,
          .runnerUp = std::nullopt};
}

}
