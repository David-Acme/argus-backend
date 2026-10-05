#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/visitor/services/visit-pattern.hxx>
#include <feature/visitor/services/visitor-policy.hxx>

#include <array>
#include <ctime>
#include <optional>
#include <vector>

namespace
{
FaceQuality goodFace()
{
  return {.detectorScore = 0.99F,
          .faceWidthPx = 60.0F,
          .interOcularPx = 28.0F,
          .yaw = 0.05F,
          .pitch = 0.5F,
          .sharpness = 500.0F};
}

SightingDecision decide(std::vector<PoolNeighbour> neighbours,
                        const FaceQuality& quality = goodFace(),
                        bool enabled = true)
{
  const auto thresholds = visitor_policy::defaults();
  return visitor_policy::decide({.neighbours = neighbours,
                                 .quality = quality,
                                 .recognitionEnabled = enabled,
                                 .thresholds = thresholds});
}

struct LocalMoment
{
  int weekday{0};
  int hour{0};
  int week{0};
};

int64_t localStamp(const LocalMoment& moment)
{
  std::tm base{};
  base.tm_year = 2026 - 1900;
  base.tm_mon = 8;
  base.tm_mday = 6 + moment.weekday + 7 * moment.week;
  base.tm_hour = moment.hour;
  base.tm_min = 10;
  base.tm_isdst = -1;
  return static_cast<int64_t>(std::mktime(&base));
}
}

TEST_CASE("a household member is recognised and never lands in the visitor pool")
{
  const auto match = decide({{.personId = 1, .score = 0.62F, .pool = PersonPool::Household},
                             {.personId = 9, .score = 0.40F, .pool = PersonPool::Unnamed}});
  CHECK(match.outcome == SightingOutcome::Household);
  CHECK(match.personId == 1);
  CHECK_FALSE(match.learn);

  const auto near = decide({{.personId = 1, .score = 0.35F, .pool = PersonPool::Household}});
  CHECK(near.outcome == SightingOutcome::NearHousehold);
  CHECK(near.personId == 0);

  const auto nearButVisitorCloser =
      decide({{.personId = 1, .score = 0.33F, .pool = PersonPool::Household},
              {.personId = 9, .score = 0.70F, .pool = PersonPool::Unnamed}});
  CHECK(nearButVisitorCloser.outcome == SightingOutcome::NearHousehold);
}

TEST_CASE("visitors are matched with a margin and new ones need a clear face")
{
  const auto visitor = decide({{.personId = 9, .score = 0.66F, .pool = PersonPool::Named}});
  CHECK(visitor.outcome == SightingOutcome::Visitor);
  CHECK(visitor.learn);

  const auto ambiguous =
      decide({{.personId = 9, .score = 0.66F, .pool = PersonPool::Unnamed},
              {.personId = 10, .score = 0.62F, .pool = PersonPool::Unnamed}});
  CHECK(ambiguous.outcome == SightingOutcome::Ambiguous);

  const auto uncertain = decide({{.personId = 9, .score = 0.40F, .pool = PersonPool::Unnamed}});
  CHECK(uncertain.outcome == SightingOutcome::Uncertain);

  const auto fresh = decide({{.personId = 9, .score = 0.10F, .pool = PersonPool::Unnamed}});
  CHECK(fresh.outcome == SightingOutcome::NewVisitor);
  CHECK(fresh.learn);

  FaceQuality small = goodFace();
  small.interOcularPx = 14.0F;
  CHECK(decide({}, small).outcome == SightingOutcome::LowQuality);
  CHECK(decide({{.personId = 9, .score = 0.70F, .pool = PersonPool::Unnamed}}, small)
            .outcome == SightingOutcome::Visitor);
  CHECK_FALSE(decide({{.personId = 9, .score = 0.70F, .pool = PersonPool::Unnamed}}, small)
                  .learn);

  FaceQuality turned = goodFace();
  turned.yaw = 0.5F;
  CHECK(decide({}, turned).outcome == SightingOutcome::LowQuality);
  FaceQuality tiny = goodFace();
  tiny.interOcularPx = 8.0F;
  CHECK(decide({{.personId = 1, .score = 0.9F, .pool = PersonPool::Household}}, tiny)
            .outcome == SightingOutcome::LowQuality);
}

TEST_CASE("with recognition off only the household is recognised")
{
  CHECK(decide({{.personId = 9, .score = 0.9F, .pool = PersonPool::Named}}, goodFace(), false)
            .outcome == SightingOutcome::Disabled);
  CHECK(decide({}, goodFace(), false).outcome == SightingOutcome::Disabled);
  CHECK(decide({{.personId = 1, .score = 0.7F, .pool = PersonPool::Household}}, goodFace(),
               false)
            .outcome == SightingOutcome::Household);
}

TEST_CASE("samples are kept best-K with outliers rejected")
{
  const auto thresholds = visitor_policy::defaults();
  CHECK(visitor_policy::admit({.similarities = {},
                               .existingQualities = {},
                               .quality = 0.5F,
                               .thresholds = thresholds})
            .kind == SampleAdmissionKind::Add);
  const std::array<float, 3> foreign{0.1F, 0.2F, 0.6F};
  const std::array<float, 3> some{0.5F, 0.6F, 0.7F};
  CHECK(visitor_policy::admit({.similarities = foreign,
                               .existingQualities = some,
                               .quality = 0.9F,
                               .thresholds = thresholds})
            .kind == SampleAdmissionKind::Outlier);
  const std::vector<float> full(8, 0.7F);
  std::vector<float> qualities{0.6F, 0.5F, 0.2F, 0.7F, 0.8F, 0.6F, 0.9F, 0.4F};
  const auto replace = visitor_policy::admit({.similarities = full,
                                              .existingQualities = qualities,
                                              .quality = 0.55F,
                                              .thresholds = thresholds});
  CHECK(replace.kind == SampleAdmissionKind::Replace);
  CHECK(replace.replaceIndex == 2);
  CHECK(visitor_policy::admit({.similarities = full,
                               .existingQualities = qualities,
                               .quality = 0.1F,
                               .thresholds = thresholds})
            .kind == SampleAdmissionKind::Skip);

  const std::array<float, 3> repeated{0.6F, 0.97F, 0.5F};
  const std::array<float, 3> repeatedQualities{0.5F, 0.6F, 0.7F};
  const auto sharper = visitor_policy::admit({.similarities = repeated,
                                              .existingQualities = repeatedQualities,
                                              .quality = 0.9F,
                                              .thresholds = thresholds});
  CHECK(sharper.kind == SampleAdmissionKind::Replace);
  CHECK(sharper.replaceIndex == 1);
  CHECK(visitor_policy::admit({.similarities = repeated,
                               .existingQualities = repeatedQualities,
                               .quality = 0.3F,
                               .thresholds = thresholds})
            .kind == SampleAdmissionKind::Skip);
}

TEST_CASE("a visit pattern names the weekday and hour someone usually comes")
{
  std::vector<int64_t> tuesdays;
  tuesdays.reserve(5);
  for (int week = 0; week < 4; ++week)
    tuesdays.push_back(localStamp({.weekday = 2, .hour = 10, .week = week}));
  tuesdays.push_back(localStamp({.weekday = 4, .hour = 11, .week = 0}));
  const auto pattern = visit_pattern::summarize(tuesdays);
  REQUIRE(pattern.weekdays.size() == 1);
  CHECK(pattern.weekdays.front() == 2);
  CHECK(pattern.usualHour == std::optional<int>{10});

  const std::array<int64_t, 2> few{localStamp({.weekday = 1, .hour = 9, .week = 0}), localStamp({.weekday = 1, .hour = 9, .week = 1})};
  CHECK(visit_pattern::summarize(few).weekdays.empty());

  std::vector<int64_t> scattered;
  scattered.reserve(7);
  for (int day = 0; day < 7; ++day)
    scattered.push_back(localStamp({.weekday = day, .hour = day * 3, .week = 0}));
  const auto none = visit_pattern::summarize(scattered);
  CHECK(none.weekdays.empty());
  CHECK_FALSE(none.usualHour.has_value());
}
