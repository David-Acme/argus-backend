#pragma once

#include <auth/module-snapshot.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <json/value.h>
#include <mcp/tool.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slots
{

struct TextRequest
{
  std::string_view tool;
  std::string_view field;
  std::string_view utterance;
  std::string_view lang;
};

class TextSlots
{
public:
  TextSlots() = default;
  virtual ~TextSlots() = default;
  TextSlots(const TextSlots&) = delete;
  TextSlots& operator=(const TextSlots&) = delete;

  [[nodiscard]] virtual std::optional<std::string> extract(const TextRequest& request) const = 0;
};

class RuleText final : public TextSlots
{
public:
  [[nodiscard]] std::optional<std::string> extract(const TextRequest& request) const override;
};

struct FillInput
{
  const argus::mcp::ToolSpec& spec;
  const std::vector<std::string>& fields;
  Json::Value arguments;
  const tools::ToolContext& context;
  int64_t now{0};
  const TextSlots& text;
  const ModuleSnapshot& modules;
  bool answering{false};
};

struct Filled
{
  Json::Value arguments;
  std::vector<std::string> missing;
};

[[nodiscard]] Filled fill(const FillInput& input);

[[nodiscard]] std::optional<std::string> answerText(std::string_view utterance);

[[nodiscard]] std::string capitalized(std::string text);

[[nodiscard]] bool namesOther(std::string_view utterance);

[[nodiscard]] bool namesNewOne(std::string_view utterance);

enum class ChoiceKind : std::uint8_t
{
  Chosen,
  Ambiguous,
  Unknown
};

struct Choice
{
  ChoiceKind kind{ChoiceKind::Unknown};
  std::string name;
};

[[nodiscard]] Choice choose(std::string_view utterance, const std::vector<std::string>& options);

[[nodiscard]] std::optional<std::string> nameGiven(std::string_view utterance);

[[nodiscard]] bool isDateTime(const argus::mcp::ToolSpec& spec, std::string_view field);

}
