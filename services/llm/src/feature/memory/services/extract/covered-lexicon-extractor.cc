#include "covered-lexicon-extractor.hxx"

#include <config/config-service.hxx>
#include <string_view>

namespace
{

size_t wordCount(std::string_view text)
{
  size_t count = 0;
  bool inWord = false;
  for (const char c : text) {
    const bool space = c == ' ' || c == '\t' || c == '\n';
    if (space) {
      inWord = false;
      continue;
    }
    if (!inWord) {
      inWord = true;
      ++count;
    }
  }
  return count;
}

bool covers(std::string_view clause, const extract::ExtractedFact& fact)
{
  const size_t total = wordCount(clause);
  if (total == 0)
    return true;
  const size_t taken = wordCount(fact.subject) + wordCount(fact.predicate) +
                       wordCount(fact.value) + wordCount(fact.when.surface);
  static const double floor = [] {
    const double ratio =
        ConfigService::getDouble("extract.lexicon_min_coverage");
    return ratio > 0.0 ? ratio : 0.6;
  }();
  return static_cast<double>(taken) / static_cast<double>(total) >= floor;
}

}

void CoveredLexiconExtractor::rebuild(
    const std::vector<extract::LexiconEntry>& entries)
{
  lexicon_.rebuild(entries);
}

bool CoveredLexiconExtractor::extract(
    const extract::ExtractInput& input,
    std::vector<extract::ExtractedFact>& out) const
{
  if (!lexicon_.extract(input, out))
    return false;
  if (covers(input.clause, out.front()))
    return true;
  out.clear();
  return false;
}
