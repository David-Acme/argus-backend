#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <audio/audio-resampler.hxx>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace
{

std::vector<int16_t> tone(int rate, double hz, size_t count)
{
  std::vector<int16_t> samples(count);
  for (size_t i = 0; i < count; ++i)
    samples[i] = static_cast<int16_t>(
        10000.0 * std::sin(2.0 * std::numbers::pi * hz *
                           static_cast<double>(i) / rate));
  return samples;
}

double rmsErrorAgainstTone(const std::vector<int16_t>& out, int rate,
                           double hz)
{
  double sum = 0.0;
  size_t counted = 0;
  for (size_t k = 64; k + 64 < out.size(); ++k) {
    const double expected =
        10000.0 * std::sin(2.0 * std::numbers::pi * hz *
                           static_cast<double>(k) / rate);
    const double diff = static_cast<double>(out[k]) - expected;
    sum += diff * diff;
    ++counted;
  }
  return std::sqrt(sum / static_cast<double>(counted));
}

}

TEST_CASE("common voice rates keep a tone within one percent")
{
  for (const int source : {44100, 22050, 48000, 8000}) {
    AudioResampler resampler({.sourceRate = source, .targetRate = 16000});
    const auto input = tone(source, 440.0, static_cast<size_t>(source) * 2);
    const auto out = resampler.process(input.data(), input.size());
    CAPTURE(source);
    const auto filterDelay = static_cast<size_t>(32 * 16000 / source + 2);
    CHECK(out.size() + filterDelay >= static_cast<size_t>(16000 * 2));
    CHECK(rmsErrorAgainstTone(out, 16000, 440.0) < 100.0);
  }
}

TEST_CASE("chunked input gives the same samples as one block")
{
  const auto input = tone(22050, 440.0, 22050);
  AudioResampler whole({.sourceRate = 22050, .targetRate = 16000});
  const auto expected = whole.process(input.data(), input.size());

  AudioResampler chunked({.sourceRate = 22050, .targetRate = 16000});
  std::vector<int16_t> joined;
  for (size_t at = 0; at < input.size(); at += 4096) {
    const size_t count = std::min<size_t>(4096, input.size() - at);
    const auto part = chunked.process(input.data() + at, count);
    joined.insert(joined.end(), part.begin(), part.end());
  }
  CHECK(joined == expected);
}

TEST_CASE("ten minutes of streaming keep the output count exact")
{
  AudioResampler resampler({.sourceRate = 44100, .targetRate = 16000});
  const auto second = tone(44100, 440.0, 44100);
  size_t produced = 0;
  for (int i = 0; i < 600; ++i)
    produced += resampler.process(second.data(), second.size()).size();
  const auto expected = static_cast<size_t>(16000) * 600;
  CHECK(produced + 40 >= expected);
  CHECK(produced <= expected);
}
