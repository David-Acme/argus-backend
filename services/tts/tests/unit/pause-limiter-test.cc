#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <feature/synthesis/infra/pocket/pause-limiter.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace
{
constexpr int kRate = 44100;
constexpr float kSilence = 0.003F;

std::vector<float> tone(std::size_t samples)
{
  std::vector<float> out(samples);
  for (std::size_t i = 0; i < samples; ++i)
    out[i] = 0.2F * std::sin(2.0F * std::numbers::pi_v<float> * 220.0F * static_cast<float>(i) / kRate);
  return out;
}

}

TEST_CASE("a pause longer than the limit is shortened to the limit, speech is untouched")
{
  std::vector<float> input = tone(kRate / 2);
  input.insert(input.end(), static_cast<std::size_t>(kRate * 1.2), 0.0F);
  const auto speech = tone(kRate / 2);
  input.insert(input.end(), speech.begin(), speech.end());

  PauseLimiter limiter({.sampleRate = kRate, .maxPauseMs = 350, .silenceRms = kSilence});
  auto out = limiter.process(input);
  const auto tail = limiter.flush();
  out.insert(out.end(), tail.begin(), tail.end());

  const auto speechSamples = static_cast<std::size_t>(kRate);
  const std::size_t limitSamples = static_cast<std::size_t>(kRate) * 350 / 1000;
  CHECK(out.size() == speechSamples + limitSamples);
}

TEST_CASE("a short natural pause passes through whole")
{
  std::vector<float> input = tone(kRate / 4);
  input.insert(input.end(), static_cast<std::size_t>(kRate * 0.15), 0.0F);
  const auto speech = tone(kRate / 4);
  input.insert(input.end(), speech.begin(), speech.end());

  PauseLimiter limiter({.sampleRate = kRate, .maxPauseMs = 350, .silenceRms = kSilence});
  auto out = limiter.process(input);
  const auto tail = limiter.flush();
  out.insert(out.end(), tail.begin(), tail.end());
  CHECK(out.size() == input.size());
}

TEST_CASE("a stream split into many calls comes out the same as one call")
{
  std::vector<float> input = tone(kRate / 3);
  input.insert(input.end(), static_cast<std::size_t>(kRate * 0.9), 0.0F);
  const auto speech = tone(kRate / 3);
  input.insert(input.end(), speech.begin(), speech.end());

  PauseLimiter whole({.sampleRate = kRate, .maxPauseMs = 350, .silenceRms = kSilence});
  auto expected = whole.process(input);
  const auto expectedTail = whole.flush();
  expected.insert(expected.end(), expectedTail.begin(), expectedTail.end());

  PauseLimiter split({.sampleRate = kRate, .maxPauseMs = 350, .silenceRms = kSilence});
  std::vector<float> actual;
  for (std::size_t at = 0; at < input.size(); at += 1000) {
    const std::size_t count = std::min<std::size_t>(1000, input.size() - at);
    const auto part = split.process(std::span<const float>(input.data() + at, count));
    actual.insert(actual.end(), part.begin(), part.end());
  }
  const auto tail = split.flush();
  actual.insert(actual.end(), tail.begin(), tail.end());
  CHECK(actual == expected);
}
