#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/gliner-decoder.hxx>

#include <json/reader.h>
#include <json/value.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace
{
float logitOf(float probability)
{
  return std::log(probability / (1.0F - probability));
}

Json::Value readJson(const char* path)
{
  std::ifstream in(path);
  Json::Value root;
  Json::CharReaderBuilder builder;
  std::string errors;
  Json::parseFromStream(builder, in, &root, &errors);
  return root;
}

struct Expectation
{
  std::int32_t start{0};
  std::int32_t end{0};
  float probability{0.0F};

  bool operator==(const Expectation&) const = default;
};

std::vector<Expectation> expectationsOf(const Json::Value& spans)
{
  std::vector<Expectation> out;
  for (const Json::Value& span : spans)
    out.push_back({.start = span[1].asInt(), .end = span[2].asInt(), .probability = span[0].asFloat()});
  return out;
}

std::vector<turn::GlinerCandidate> candidatesOf(const Json::Value& spans, std::int32_t query)
{
  std::vector<turn::GlinerCandidate> out;
  for (const Json::Value& span : spans)
    out.push_back({.query = query,
                   .start = span[1].asInt(),
                   .end = span[2].asInt(),
                   .logit = logitOf(span[0].asFloat())});
  return out;
}

void checkSame(const std::vector<turn::GlinerDecodedSpan>& got, const std::vector<Expectation>& expected)
{
  REQUIRE(got.size() == expected.size());
  for (std::size_t at = 0; at < expected.size(); ++at) {
    CHECK(got[at].start == expected[at].start);
    CHECK(got[at].end == expected[at].end);
    CHECK(got[at].probability == doctest::Approx(expected[at].probability).epsilon(1e-4F));
  }
}

turn::GlinerDecodeInput unfiltered(std::span<const turn::GlinerCandidate> candidates)
{
  return {.candidates = candidates,
          .queryThresholds = {},
          .defaultThreshold = 0.0F,
          .maxWidth = 1 << 20,
          .overlap = turn::GlinerOverlap::Flat};
}
}

TEST_CASE("a span that cannot reach its query threshold is dropped, and the rest are sigmoid-scored")
{
  const std::vector<turn::GlinerCandidate> candidates{{.query = 0, .start = 0, .end = 2, .logit = logitOf(0.9F)},
                                                      {.query = 0, .start = 3, .end = 5, .logit = logitOf(0.2F)},
                                                      {.query = 1, .start = 0, .end = 2, .logit = logitOf(0.4F)}};
  const std::array<float, 2> thresholds{0.5F, 0.3F};
  const std::vector<turn::GlinerDecodedSpan> spans = turn::glinerDecode({.candidates = candidates,
                                                                         .queryThresholds = thresholds,
                                                                         .defaultThreshold = 0.5F,
                                                                         .maxWidth = 8,
                                                                         .overlap = turn::GlinerOverlap::Flat});
  REQUIRE(spans.size() == 2);
  CHECK(spans[0].query == 0);
  CHECK(spans[0].start == 0);
  CHECK(spans[0].end == 2);
  CHECK(spans[0].probability == doctest::Approx(0.9F).epsilon(0.001F));
  CHECK(spans[1].query == 1);
  CHECK(spans[1].probability == doctest::Approx(0.4F).epsilon(0.001F));
}

TEST_CASE("a span wider than the maximum is never decoded")
{
  const std::vector<turn::GlinerCandidate> candidates{{.query = 0, .start = 0, .end = 9, .logit = 10.0F}};
  const std::vector<turn::GlinerDecodedSpan> spans =
      turn::glinerDecode({.candidates = candidates, .queryThresholds = {}, .maxWidth = 8, .overlap = turn::GlinerOverlap::Flat});
  CHECK(spans.empty());
}

TEST_CASE("the flat policy keeps the heaviest non-overlapping set, and the longest policy drops what is contained")
{
  const std::vector<turn::GlinerCandidate> candidates{{.query = 0, .start = 0, .end = 2, .logit = logitOf(0.9F)},
                                                      {.query = 0, .start = 1, .end = 5, .logit = logitOf(0.6F)},
                                                      {.query = 0, .start = 2, .end = 5, .logit = logitOf(0.5F)}};
  const std::vector<turn::GlinerDecodedSpan> flat =
      turn::glinerDecode({.candidates = candidates, .queryThresholds = {}, .maxWidth = 8, .overlap = turn::GlinerOverlap::Flat});
  REQUIRE(flat.size() == 2);
  CHECK(flat[0].start == 0);
  CHECK(flat[0].end == 2);
  CHECK(flat[1].start == 2);
  CHECK(flat[1].end == 5);
  const std::vector<turn::GlinerDecodedSpan> longest =
      turn::glinerDecode({.candidates = candidates, .queryThresholds = {}, .maxWidth = 8, .overlap = turn::GlinerOverlap::Longest});
  REQUIRE(longest.size() == 2);
  CHECK(longest[0].start == 0);
  CHECK(longest[0].end == 2);
  CHECK(longest[1].start == 1);
  CHECK(longest[1].end == 5);
  const std::vector<turn::GlinerDecodedSpan> allow =
      turn::glinerDecode({.candidates = candidates, .queryThresholds = {}, .maxWidth = 8, .overlap = turn::GlinerOverlap::Allow});
  CHECK(allow.size() == 3);
}

TEST_CASE("spans that do not touch are all kept, ranked by confidence then position")
{
  const std::vector<turn::GlinerCandidate> candidates{{.query = 0, .start = 5, .end = 6, .logit = logitOf(0.7F)},
                                                      {.query = 0, .start = 0, .end = 1, .logit = logitOf(0.7F)},
                                                      {.query = 0, .start = 2, .end = 3, .logit = logitOf(0.95F)}};
  const std::vector<turn::GlinerDecodedSpan> spans =
      turn::glinerDecode({.candidates = candidates, .queryThresholds = {}, .maxWidth = 8, .overlap = turn::GlinerOverlap::Flat});
  REQUIRE(spans.size() == 3);
  CHECK(spans[0].start == 2);
  CHECK(spans[1].start == 0);
  CHECK(spans[2].start == 5);
}

TEST_CASE("token boundaries map to characters with the half-open rule")
{
  const std::array<std::int32_t, 4> starts{0, 4, 9, 12};
  const std::array<std::int32_t, 4> ends{3, 8, 11, 15};
  const turn::GlinerDecodedSpan span{.query = 0, .start = 1, .end = 3, .probability = 0.9F};
  const turn::GlinerOffsets offsets = turn::glinerCharacterOffsets(span, starts, ends);
  CHECK(offsets.begin == 4);
  CHECK(offsets.end == 11);
  const turn::GlinerOffsets empty =
      turn::glinerCharacterOffsets({.query = 0, .start = 2, .end = 2, .probability = 1.0F}, starts, ends);
  CHECK(empty.begin == 0);
  CHECK(empty.end == 0);
}

TEST_CASE("the flat policy reproduces the reference on the frozen overlap rows")
{
  const Json::Value fixture = readJson(ARGUS_TEST_GLINER_OVERLAP);
  REQUIRE(fixture["cases"].size() >= 8);
  std::size_t queries = 0;
  std::size_t discriminating = 0;
  for (const Json::Value& item : fixture["cases"]) {
    for (Json::Value::ArrayIndex index = 0; index < item["queries"].size(); ++index) {
      const Json::Value& query = item["queries"][index];
      const std::vector<turn::GlinerCandidate> candidates =
          candidatesOf(query["candidates"], static_cast<std::int32_t>(index));
      const std::vector<Expectation> expected = expectationsOf(query["expected"]);
      if (expectationsOf(query["greedy"]) != expected)
        ++discriminating;
      checkSame(turn::glinerDecode(unfiltered(candidates)), expected);
      ++queries;
    }
  }
  MESSAGE("gliner overlap rows: " << queries << " queries over " << fixture["cases"].size() << " cases, " << discriminating
                                  << " where greedy first-wins and the reference differ");
  CHECK(queries >= 8);
  CHECK(discriminating == fixture["greedyDiffers"].asUInt());
}

TEST_CASE("the flat policy reproduces the reference over random interval sets, where greedy does not")
{
  const Json::Value fixture = readJson(ARGUS_TEST_GLINER_RANDOM);
  REQUIRE(fixture["cases"].size() >= 200);
  std::size_t differing = 0;
  for (const Json::Value& item : fixture["cases"]) {
    const std::vector<turn::GlinerCandidate> candidates = candidatesOf(item["spans"], 0);
    const std::vector<Expectation> expected = expectationsOf(item["expected"]);
    if (expectationsOf(item["greedy"]) != expected)
      ++differing;
    checkSame(turn::glinerDecode(unfiltered(candidates)), expected);
  }
  MESSAGE("gliner random intervals: " << fixture["cases"].size() << " sets, " << differing
                                      << " where greedy first-wins differs from the reference");
  CHECK(differing == fixture["greedyDiffers"].asUInt());
  CHECK(differing > 0);
}
