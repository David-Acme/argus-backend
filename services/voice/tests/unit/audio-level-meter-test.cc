#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <feature/rtc/audio-level-meter.hxx>

#include <vector>

TEST_CASE("silence is reported as the floor, one report per interval, with the frame count")
{
  AudioLevelMeter meter(1000);
  const std::vector<int16_t> silent(480, 0);
  CHECK_FALSE(meter.add(silent, 0).has_value());
  CHECK_FALSE(meter.add(silent, 500).has_value());
  const auto report = meter.add(silent, 1000);
  REQUIRE(report.has_value());
  CHECK(report->frames == 3);
  CHECK(report->samples == 3 * 480);
  CHECK(report->rmsDbfs == -120.0);
  CHECK(report->peak == 0);
}

TEST_CASE("a full-scale square wave reads near 0 dBFS, and the window restarts after each report")
{
  AudioLevelMeter meter(1000);
  const std::vector<int16_t> loud(480, 32767);
  const auto first = meter.add(loud, 0);
  CHECK_FALSE(first.has_value());
  const auto report = meter.add(loud, 1000);
  REQUIRE(report.has_value());
  CHECK(report->rmsDbfs > -0.1);
  CHECK(report->peak == 32767);
  CHECK_FALSE(meter.add(std::vector<int16_t>(480, 0), 1500).has_value());
}
