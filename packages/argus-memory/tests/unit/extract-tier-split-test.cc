#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/extract/lexicon-extractor.hxx>
#include <shared/services/extract/vocabulary-lexicon.hxx>
#include <shared/services/memory/phrase-catalog.hxx>
#include <shared/services/memory/rule-parser.hxx>

#include <fstream>
#include <string>
#include <vector>

namespace
{

#ifndef ARGUS_TEST_MEMORY_FIXTURES_DIR
#define ARGUS_TEST_MEMORY_FIXTURES_DIR "packages/argus-memory/tests/fixtures"
#endif

// Regression floor: every point it loses wakes the 1.2 s model tier instead.
constexpr double kMinLexiconHitRate = 0.60;

std::vector<std::string> savedUtterances(const std::string& path)
{
  std::vector<std::string> rows;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const auto tab = line.find('\t');
    if (tab == std::string::npos || tab + 1 >= line.size())
      continue;
    if (line.compare(0, tab, "memory_save") == 0)
      rows.push_back(line.substr(tab + 1));
  }
  return rows;
}

} // namespace

// Reports the share of real memory_save utterances that would wake the model tier.
TEST_CASE("the lexicon tier carries most real memory_save turns")
{
  const auto rows = savedUtterances(
      std::string(ARGUS_TEST_MEMORY_FIXTURES_DIR) + "/eval-usage.tsv");
  REQUIRE(rows.size() > 0);

  PhraseCatalog catalog;
  catalog.build();
  const RuleParser parser(catalog);
  LexiconExtractor lexicon;
  lexicon.rebuild(extract::allLexiconEntries());

  int reachedExtraction = 0;
  int lexiconHits = 0;
  int noClause = 0;
  for (const auto& utterance : rows) {
    const RuleParseInput input{.text = utterance, .lang = "es"};
    const auto parsed = parser.parse(input);
    const auto statement = parsed ? std::nullopt : parser.parseStatement(input);
    if (!parsed && !statement)
      ++noClause;

    // A routed turn is salient, so the whole utterance is the clause then.
    const std::string clause =
        parser.stripFillers({.text = parsed    ? parsed->content
                                     : statement ? statement->content
                                                 : utterance,
                             .lang = "es"});
    if (clause.empty())
      continue;
    ++reachedExtraction;

    std::vector<extract::ExtractedFact> facts;
    lexicon.extract({.clause = clause,
                     .lang = "es",
                     .userId = 0,
                     .requireModel = false,
                     .allowModel = false},
                    facts);
    if (!facts.empty())
      ++lexiconHits;
  }
  REQUIRE(reachedExtraction > 0);

  const double hitRate = static_cast<double>(lexiconHits) /
                         static_cast<double>(reachedExtraction);
  MESSAGE("lexicon " << lexiconHits << "/" << reachedExtraction << " reaching "
                     << "extraction (" << (hitRate * 100.0) << "% hit); "
                     << noClause << "/" << rows.size()
                     << " carry no rule clause and rely on the routed "
                     << "salient path");
  CHECK(hitRate >= kMinLexiconHitRate);
}
