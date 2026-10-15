#include "opening-budget.hxx"

#include <utility>

std::size_t utf8Length(const std::string& text)
{
  std::size_t count = 0;
  for (const unsigned char byte : text)
    if ((byte & 0xC0) != 0x80)
      ++count;
  return count;
}

std::vector<std::string> splitSentences(const std::string& text)
{
  std::vector<std::string> out;
  std::string current;
  for (std::size_t i = 0; i < text.size(); ++i) {
    current.push_back(text[i]);
    const bool ends = text[i] == '.' || text[i] == '!' || text[i] == '?';
    const bool boundary = i + 1 == text.size() || text[i + 1] == ' ';
    if (ends && boundary) {
      out.push_back(current);
      current.clear();
    }
  }
  if (!current.empty())
    out.push_back(current);
  for (auto& sentence : out) {
    while (!sentence.empty() && sentence.front() == ' ')
      sentence.erase(sentence.begin());
  }
  return out;
}

OpeningBudgetResult fitOpening(const OpeningBudgetInput& input)
{
  OpeningBudgetResult result;
  double rate = input.estimateMsPerChar;
  double used = 0.0;
  std::size_t next = 0;
  for (; next < input.sentences.size(); ++next) {
    const std::string& sentence = input.sentences[next];
    const double estimate = rate * static_cast<double>(utf8Length(sentence));
    if (!result.spoken.empty() && used + estimate > static_cast<double>(input.budgetMs))
      break;
    const int actual = input.synthesizeMs(sentence);
    const auto chars = utf8Length(sentence);
    if (chars > 0)
      rate = static_cast<double>(actual) / static_cast<double>(chars);
    used += static_cast<double>(actual);
    result.spoken.push_back(sentence);
    if (used >= static_cast<double>(input.budgetMs)) {
      ++next;
      break;
    }
  }
  for (; next < input.sentences.size(); ++next)
    result.later.push_back(input.sentences[next]);
  result.msPerChar = rate;
  return result;
}
