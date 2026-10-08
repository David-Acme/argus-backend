#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/rtc/playout-gain.hxx>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace
{

constexpr int16_t kLoud = 10000;

std::vector<int16_t> frame()
{
  return std::vector<int16_t>(160, kLoud);
}

}

TEST_CASE("an unducked voice passes untouched")
{
  PlayoutGain gain;
  auto samples = frame();
  gain.apply(samples);
  CHECK(samples.front() == kLoud);
  CHECK(samples.back() == kLoud);
  CHECK(gain.current() == doctest::Approx(1.0F));
}

TEST_CASE("ducking lowers the voice smoothly to the duck level and holds it")
{
  PlayoutGain gain({.duckGain = 0.3F, .duckFrames = 4, .releaseFrames = 8});
  gain.duck(true);
  auto first = frame();
  gain.apply(first);
  CHECK(first.front() == kLoud);
  CHECK(first.back() == static_cast<int16_t>(kLoud * 0.825F));
  CHECK(first.front() > first.back());
  for (int i = 0; i < 10; ++i) {
    auto samples = frame();
    gain.apply(samples);
  }
  auto held = frame();
  gain.apply(held);
  CHECK(gain.current() == doctest::Approx(0.3F));
  CHECK(held.front() == static_cast<int16_t>(kLoud * 0.3F));
}

TEST_CASE("a released duck climbs back slower than it fell")
{
  PlayoutGain gain({.duckGain = 0.3F, .duckFrames = 2, .releaseFrames = 7});
  gain.duck(true);
  for (int i = 0; i < 2; ++i) {
    auto samples = frame();
    gain.apply(samples);
  }
  CHECK(gain.current() == doctest::Approx(0.3F));
  gain.duck(false);
  auto samples = frame();
  gain.apply(samples);
  CHECK(gain.current() == doctest::Approx(0.4F));
  for (int i = 0; i < 6; ++i) {
    auto more = frame();
    gain.apply(more);
  }
  CHECK(gain.current() == doctest::Approx(1.0F));
}

TEST_CASE("a fade out ends in silence from wherever the voice was and leaves the next turn at full volume")
{
  PlayoutGain gain({.duckGain = 0.3F, .duckFrames = 1, .releaseFrames = 10});
  gain.duck(true);
  auto ducked = frame();
  gain.apply(ducked);
  std::vector<int16_t> tail(480, kLoud);
  gain.fadeOut(tail);
  CHECK(tail.front() == static_cast<int16_t>(kLoud * 0.3F));
  CHECK(tail.back() == 0);
  CHECK(gain.current() == doctest::Approx(1.0F));
  CHECK(gain.target() == doctest::Approx(1.0F));
}

TEST_CASE("the first faded sample continues at the gain the voice was playing at")
{
  PlayoutGain gain({.duckGain = 0.3F, .duckFrames = 2, .releaseFrames = 8});
  gain.duck(true);
  auto ducked = frame();
  gain.apply(ducked);
  const float current = gain.current();
  CHECK(current == doctest::Approx(0.65F));
  std::vector<int16_t> tail(480, static_cast<int16_t>(9999));
  gain.fadeOut(tail);
  const auto expected =
      static_cast<int16_t>(std::lround(static_cast<float>(9999) * current));
  CHECK(std::abs(static_cast<int>(tail.front()) - static_cast<int>(expected)) <= 1);
  CHECK(tail.back() == 0);
}
