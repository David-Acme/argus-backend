#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace turn
{

class SpTokenizer;

enum class QuestionKind : unsigned char
{
  Choice = 0,
  Score = 1,
  Noul = 2
};

struct LayaQuestion
{
  QuestionKind kind{QuestionKind::Choice};
  std::string instructions;
  std::vector<std::pair<std::string, std::string>> criteria;
  std::string falseLabel{"false"};
  std::string trueLabel{"true"};
};

struct LayaSequence
{
  std::vector<std::int32_t> ids;
  std::vector<std::size_t> markers;
};

struct LayaSequenceInput
{
  const SpTokenizer& tokenizer;
  std::string_view state;
  int maxLen{512};
  int headMaxLen{192};
};

[[nodiscard]] std::vector<std::string> layaOptions(const LayaQuestion& question);

[[nodiscard]] LayaSequence layaBuildSequence(const LayaSequenceInput& input, const LayaQuestion& question);

}
