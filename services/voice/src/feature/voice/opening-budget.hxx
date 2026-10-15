#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

inline constexpr int kOpeningBudgetMs = 4000;

struct OpeningBudgetInput
{
  std::vector<std::string> sentences;
  int budgetMs{kOpeningBudgetMs};
  double estimateMsPerChar{0.0};
  std::function<int(const std::string&)> synthesizeMs;
};

struct OpeningBudgetResult
{
  std::vector<std::string> spoken;
  std::vector<std::string> later;
  double msPerChar{0.0};
};

std::vector<std::string> splitSentences(const std::string& text);
std::size_t utf8Length(const std::string& text);
OpeningBudgetResult fitOpening(const OpeningBudgetInput& input);
