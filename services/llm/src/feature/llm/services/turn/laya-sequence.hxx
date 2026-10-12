#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
  std::vector<std::string> labels;
  std::vector<std::string> options;
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

[[nodiscard]] std::optional<LayaQuestion> layaQuestionFromJson(const nlohmann::ordered_json& definition);

[[nodiscard]] std::string layaPythonJson(const nlohmann::ordered_json& value);

[[nodiscard]] LayaSequence layaBuildSequence(const LayaSequenceInput& input, const LayaQuestion& question);

}
