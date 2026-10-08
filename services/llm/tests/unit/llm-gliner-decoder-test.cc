#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/turn/gliner-decoder.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace
{
float logitOf(float probability)
{
  return std::log(probability / (1.0F - probability));
}
}

TEST_CASE("a span below its query threshold is dropped, and the rest are sigmoid-scored")
{
  const std::vector<turn::GlinerCandidate> candidates{{.query = 0, .start = 0, .end = 2, .logit = logitOf(0.9F)},
                                                      {.query = 0, .start = 3, .end = 5, .logit = logitOf(0.2F)},
                                                      {.query = 1, .start = 0, .end = 2, .logit = logitOf(0.4F)}};
  const std::array<float, 2> thresholds{0.5F, 0.3F};
  const std::vector<turn::GlinerSpan> spans = turn::glinerDecode({.candidates = candidates,
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
  const std::vector<turn::GlinerSpan> spans =
      turn::glinerDecode({.candidates = candidates, .maxWidth = 8, .overlap = turn::GlinerOverlap::Flat});
  CHECK(spans.empty());
}

TEST_CASE("flat overlap resolution keeps the higher scoring of two overlapping spans")
{
  const std::vector<turn::GlinerCandidate> candidates{{.query = 0, .start = 0, .end = 2, .logit = logitOf(0.9F)},
                                                      {.query = 0, .start = 1, .end = 5, .logit = logitOf(0.6F)}};
  const std::vector<turn::GlinerSpan> flat =
      turn::glinerDecode({.candidates = candidates, .maxWidth = 8, .overlap = turn::GlinerOverlap::Flat});
  REQUIRE(flat.size() == 1);
  CHECK(flat[0].start == 0);
  CHECK(flat[0].end == 2);
  const std::vector<turn::GlinerSpan> longest =
      turn::glinerDecode({.candidates = candidates, .maxWidth = 8, .overlap = turn::GlinerOverlap::Longest});
  REQUIRE(longest.size() == 1);
  CHECK(longest[0].start == 1);
  CHECK(longest[0].end == 5);
  const std::vector<turn::GlinerSpan> allow =
      turn::glinerDecode({.candidates = candidates, .maxWidth = 8, .overlap = turn::GlinerOverlap::Allow});
  CHECK(allow.size() == 2);
}

TEST_CASE("spans that do not touch are all kept, ranked by confidence then position")
{
  const std::vector<turn::GlinerCandidate> candidates{{.query = 0, .start = 5, .end = 6, .logit = logitOf(0.7F)},
                                                      {.query = 0, .start = 0, .end = 1, .logit = logitOf(0.7F)},
                                                      {.query = 0, .start = 2, .end = 3, .logit = logitOf(0.95F)}};
  const std::vector<turn::GlinerSpan> spans =
      turn::glinerDecode({.candidates = candidates, .maxWidth = 8, .overlap = turn::GlinerOverlap::Flat});
  REQUIRE(spans.size() == 3);
  CHECK(spans[0].start == 2);
  CHECK(spans[1].start == 0);
  CHECK(spans[2].start == 5);
}

TEST_CASE("token boundaries map to characters with the half-open rule")
{
  const std::array<std::int32_t, 4> starts{0, 4, 9, 12};
  const std::array<std::int32_t, 4> ends{3, 8, 11, 15};
  const turn::GlinerSpan span{.query = 0, .start = 1, .end = 3, .probability = 0.9F};
  const turn::GlinerOffsets offsets = turn::glinerCharacterOffsets(span, starts, ends);
  CHECK(offsets.begin == 4);
  CHECK(offsets.end == 11);
  const turn::GlinerOffsets empty = turn::glinerCharacterOffsets({.query = 0, .start = 2, .end = 2, .probability = 1.0F}, starts, ends);
  CHECK(empty.begin == 0);
  CHECK(empty.end == 0);
}
