#pragma once

#include "speech-acts.hxx"

#include <string>
#include <string_view>

namespace turn::speech
{

struct RenderInput
{
  const Speech& speech;
  std::string_view contextBlock;
};

[[nodiscard]] std::string actTail(const RenderInput& input);

[[nodiscard]] std::string_view instructionLine(const Act& act, std::string_view lang);

[[nodiscard]] std::string dateSurface(const DatePart& part, std::string_view lang, int64_t now);

}
