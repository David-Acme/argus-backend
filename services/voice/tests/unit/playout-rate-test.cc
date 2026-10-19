#include <doctest/doctest.h>
#include <audio/audio-resampler.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace
{
constexpr double kPi = std::numbers::pi;
constexpr int kTtsRate = 24000;
constexpr int kPlayoutRate = 48000;
constexpr int kUplinkRate = 16000;
constexpr double kToneHz = 1000.0;
constexpr double kAmplitude = 8000.0;
constexpr size_t kTtsSamples = kTtsRate;

struct ToneSpec
{
  int rate{0};
  size_t samples{0};
};

std::vector<int16_t> toneAt(ToneSpec spec)
{
  std::vector<int16_t> out(spec.samples);
  for (size_t i = 0; i < spec.samples; ++i)
    out[i] = static_cast<int16_t>(std::lround(kAmplitude * std::sin(2.0 * kPi * kToneHz * static_cast<double>(i) / spec.rate)));
  return out;
}

double bestSnrDb(const std::vector<int16_t>& got, int rate)
{
  constexpr size_t kMaxLag = 400;
  constexpr size_t kWindow = 16000;
  double best = -1000.0;
  for (size_t lag = 0; lag < kMaxLag; ++lag) {
    double signal = 0.0;
    double noise = 0.0;
    for (size_t i = 0; i < kWindow && lag + i < got.size(); ++i) {
      const double ideal = kAmplitude * std::sin(2.0 * kPi * kToneHz * static_cast<double>(i) / rate);
      const double diff = static_cast<double>(got[lag + i]) - ideal;
      signal += ideal * ideal;
      noise += diff * diff;
    }
    if (noise > 0.0)
      best = std::max(best, 10.0 * std::log10(signal / noise));
  }
  return best;
}
}

TEST_CASE("the TTS audio reaches the 48 kHz playout with one resample, cleaner than through the 16 kHz uplink rate")
{
  const auto tts = toneAt({.rate = kTtsRate, .samples = kTtsSamples});

  AudioResampler single({.sourceRate = kTtsRate, .targetRate = kPlayoutRate});
  const auto playout = single.process(tts.data(), tts.size());

  AudioResampler down({.sourceRate = kTtsRate, .targetRate = kUplinkRate});
  const auto narrow = down.process(tts.data(), tts.size());
  AudioResampler up({.sourceRate = kUplinkRate, .targetRate = kPlayoutRate});
  const auto twice = up.process(narrow.data(), narrow.size());

  CHECK(playout.size() > 2 * kTtsSamples * 99 / 100);
  CHECK(playout.size() < 2 * kTtsSamples * 101 / 100);
  const double singleDb = bestSnrDb(playout, kPlayoutRate);
  const double twiceDb = bestSnrDb(twice, kPlayoutRate);
  CHECK(singleDb > 40.0);
  CHECK(singleDb >= twiceDb - 1.0);
}
