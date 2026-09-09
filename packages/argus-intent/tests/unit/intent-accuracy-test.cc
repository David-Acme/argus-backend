#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/intent/fasttext-classifier.hxx>
#include <shared/services/intent/intent-contracts.hxx>
#include <shared/services/intent/intent-router.hxx>
#include <shared/services/memory/phrase-catalog.hxx>

#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct FixtureRow
{
  std::string label;
  std::string text;
};

struct Counts
{
  int predicted = 0;
  int correct = 0;
};

// The publish step writes "label\ttext" or "label\tlang\ttext".
std::vector<FixtureRow> readFixture(const std::string& path)
{
  std::vector<FixtureRow> rows;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    const auto first = line.find('\t');
    if (first == std::string::npos || first == 0)
      continue;
    const auto second = line.find('\t', first + 1);
    const auto textAt = second == std::string::npos ? first + 1 : second + 1;
    if (textAt >= line.size())
      continue;
    rows.push_back({.label = line.substr(0, first),
                    .text = line.substr(textAt)});
  }
  return rows;
}

// The router's operating point: below-threshold or thin-margin abstains to none.
std::string_view gatedPrediction(const std::vector<intent::IntentHit>& hits)
{
  if (hits.empty())
    return "unknown";
  const float score = hits.front().score;
  const float runner = hits.size() > 1 ? hits[1].score : 0.0F;
  if (hits.front().intent != intent::ToolIntent::None &&
      (score < IntentRouter::kThreshold ||
       (score - runner) < IntentRouter::kMargin))
    return intent::toolIntentToString(intent::ToolIntent::None);
  return intent::toolIntentToString(hits.front().intent);
}

} // namespace

// A republished model that regressed on the judges cannot enter the build.
TEST_CASE("the published model keeps memory_save precision on the judges")
{
  const FastTextClassifier model(
      std::string(ARGUS_TEST_INTENT_MODELS_DIR) + "/intent.bin");
  if (!model.isLoaded()) {
    MESSAGE("no intent model on disk; accuracy gate skipped");
    return;
  }

  for (const std::string fixture : {"eval-check.tsv", "eval-production.tsv"}) {
    const auto rows =
        readFixture(std::string(ARGUS_TEST_INTENT_FIXTURES_DIR) + "/" + fixture);
    REQUIRE(rows.size() > 0);

    std::map<std::string_view, Counts> counts;
    for (const auto& row : rows) {
      const auto hits = model.score(intent::normalizeInput(row.text));
      const std::string_view predicted = gatedPrediction(hits);
      Counts& bucket = counts[predicted];
      bucket.predicted++;
      if (predicted == row.label)
        bucket.correct++;
    }

    const Counts& save =
        counts[intent::toolIntentToString(intent::ToolIntent::MemorySave)];
    MESSAGE(fixture << ": memory_save precision " << save.correct << "/"
                    << save.predicted);
    CHECK(save.predicted > 0);
    // The 0.90 floor in integers: correct >= ceil(0.9 * predicted).
    CHECK(save.correct * 10 >= save.predicted * 9);
  }
}

// The same 20 rows the f8-b4 bench under-fired with the LLM's tool calling.
TEST_CASE("the router covers the judge set the LLM tier under-fired")
{
  const FastTextClassifier model(
      std::string(ARGUS_TEST_INTENT_MODELS_DIR) + "/intent.bin");
  if (!model.isLoaded()) {
    MESSAGE("no intent model on disk; coverage measurement skipped");
    return;
  }

  PhraseCatalog catalog;
  catalog.build();
  const IntentRouter router(
      {.catalog = catalog, .model = model, .recurrent = nullptr});

  const auto rows = readFixture(
      std::string(ARGUS_TEST_INTENT_FIXTURES_DIR) + "/eval-check.tsv");
  int saves = 0;
  int routed = 0;
  int byRules = 0;
  for (const auto& row : rows) {
    if (row.label != "memory_save")
      continue;
    ++saves;
    const auto decision = router.decide(row.text, "es");
    if (!decision.confident || decision.intent != intent::ToolIntent::MemorySave)
      continue;
    ++routed;
    if (decision.fromRules)
      ++byRules;
  }

  MESSAGE("router decides memory_save on " << routed << "/" << saves
                                           << " (" << byRules
                                           << " by rules); the f8-b4 bench "
                                           << "measured the LLM firing 9/20");
  CHECK(saves == 20);
  CHECK(routed >= 9);
}
