#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace guard_dialogue
{

struct LineInput
{
  std::string text;
  int maxWords{12};
  // Names, observations and tags the line must never echo back.
  std::vector<std::string> privateTokens;
};

// Returns a speakable line or empty when the proposal must be dropped:
// word cap, a single question, no surveillance vocabulary, no private data,
// no digits/URLs and no instruction-injection echo.
std::string sanitizeLine(const LineInput& input);

struct VariantPickInput
{
  const std::vector<std::string>& variants;
  int64_t seed{0};
  const std::string& exclude;
};

// Picks a greeting variant, skipping the one already spoken this encounter.
std::string pickVaried(const VariantPickInput& input);

} // namespace guard_dialogue
