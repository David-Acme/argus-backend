#pragma once

#include "speech-acts.hxx"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace turn::speech
{

struct RenderInput
{
  const Speech& speech;
  std::string_view contextBlock;
  bool examples{false};
};

using ExampleLine = std::pair<std::string_view, std::string_view>;

[[nodiscard]] std::string actTail(const RenderInput& input);

[[nodiscard]] std::string dateSurface(const DatePart& part, std::string_view lang, int64_t now);

[[nodiscard]] std::span<const ExampleLine> exampleLines(const Act& act, std::string_view lang);

struct RefusalSpeech
{
  const Refused& refused;
  std::string_view lang;
};

[[nodiscard]] std::optional<std::string> refusalLine(const RefusalSpeech& input);

struct SlotQuestion
{
  const AskSlot& ask;
  std::string_view lang;
};

[[nodiscard]] std::optional<std::string> slotQuestion(const SlotQuestion& input);

[[nodiscard]] std::string ungroundedLine(std::string_view lang);

}
