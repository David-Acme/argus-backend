#include "opening-budget.hxx"

#include <utility>

std::size_t utf8Length(const std::string& text)
{
  std::size_t count = 0;
  for (const unsigned char byte : text)
    if ((static_cast<unsigned>(byte) & 0xC0U) != 0x80U)
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
  if (input.sentences.empty())
    return result;
  const auto budget = static_cast<double>(input.budgetMs);
  double rate = input.estimateMsPerChar;
  double used = 0.0;
  const auto speak = [&input, &result, &rate, &used](const std::string& sentence) {
    const int actual = input.synthesizeMs(sentence);
    const std::size_t chars = utf8Length(sentence);
    if (chars > 0)
      rate = static_cast<double>(actual) / static_cast<double>(chars);
    used += static_cast<double>(actual);
    result.spoken.push_back(sentence);
  };
  const auto fits = [&rate, &used, budget](const std::string& sentence) {
    return used + rate * static_cast<double>(utf8Length(sentence)) <= budget;
  };
  speak(input.sentences.front());
  std::size_t next = 1;
  while (next < input.sentences.size() && used < budget &&
         fits(input.sentences[next])) {
    speak(input.sentences[next]);
    ++next;
  }
  for (; next < input.sentences.size(); ++next)
    result.later.push_back(input.sentences[next]);
  result.msPerChar = rate;
  return result;
}
