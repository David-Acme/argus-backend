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
  std::vector<std::string> privateTokens;
};

std::string sanitizeLine(const LineInput& input);

struct VariantPickInput
{
  const std::vector<std::string>& variants;
  int64_t seed{0};
  const std::string& exclude;
};

std::string pickVaried(const VariantPickInput& input);

}
