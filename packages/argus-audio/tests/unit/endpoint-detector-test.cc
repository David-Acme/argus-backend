#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <shared/wrapper/audio/endpoint-detector.hxx>
#include <vector>

namespace
{
constexpr int kSampleRate = 16000;
constexpr double kTwoPi = 6.283185307179586;

// A speech-like band-limited tone; its zero-crossing rate sits in the speech
// band, unlike low-frequency rumble or broadband hiss.
std::vector<int16_t> tone(int milliseconds, int16_t amplitude,
                          double frequency = 300.0)
{
  const int count = kSampleRate * milliseconds / 1000;
  std::vector<int16_t> samples(static_cast<size_t>(count));
  for (int index = 0; index < count; ++index)
    samples[static_cast<size_t>(index)] = static_cast<int16_t>(
        std::sin(kTwoPi * frequency * index / kSampleRate) * amplitude);
  return samples;
}

std::vector<int16_t> silence(int milliseconds)
{
  return std::vector<int16_t>(static_cast<size_t>(kSampleRate) * milliseconds /
                                  1000,
                              0);
}

// Stationary low-frequency rumble: loud but a very low zero-crossing rate.
std::vector<int16_t> fanNoise(int milliseconds, int16_t amplitude)
{
  return tone(milliseconds, amplitude, 25.0);
}

// Broadband hiss: loud but an out-of-band zero-crossing rate.
std::vector<int16_t> hiss(int milliseconds, int16_t amplitude)
{
  const int count = kSampleRate * milliseconds / 1000;
  std::vector<int16_t> samples(static_cast<size_t>(count));
  for (int index = 0; index < count; ++index)
    samples[static_cast<size_t>(index)] =
        (index % 2 == 0) ? amplitude : static_cast<int16_t>(-amplitude);
  return samples;
}

bool drainSilence(EndpointDetector& detector, int chunks = 40)
{
  for (int chunk = 0; chunk < chunks; ++chunk) {
    const auto trailing = silence(30);
    if (detector.process(trailing.data(), trailing.size()).endpointed)
      return true;
  }
  return false;
}

EndpointConfig baseConfig()
{
  EndpointConfig config;
  config.frameMs = 30;
  config.endSilenceMs = 150;
  config.minSpeechMs = 120;
  return config;
}
} // namespace

TEST_CASE("pure silence never starts an utterance")
{
  EndpointDetector detector(baseConfig());
  const auto samples = silence(1000);
  const EndpointStatus status =
      detector.process(samples.data(), samples.size());
  CHECK_FALSE(status.speechStarted);
  CHECK_FALSE(status.endpointed);
  CHECK_FALSE(detector.speechDetected());
}

TEST_CASE("stationary fan noise never starts an utterance")
{
  EndpointDetector detector(baseConfig());
  const auto samples = fanNoise(1000, 1500);
  const EndpointStatus status =
      detector.process(samples.data(), samples.size());
  CHECK_FALSE(status.speechStarted);
  CHECK_FALSE(detector.speechDetected());
}

TEST_CASE("broadband hiss never starts an utterance")
{
  EndpointDetector detector(baseConfig());
  const auto samples = hiss(1000, 1500);
  const EndpointStatus status =
      detector.process(samples.data(), samples.size());
  CHECK_FALSE(status.speechStarted);
  CHECK_FALSE(detector.speechDetected());
}

TEST_CASE("a stream that opens mid-speech still starts an utterance")
{
  EndpointDetector detector(baseConfig());
  const auto voiced = tone(300, 4000);
  const EndpointStatus started = detector.process(voiced.data(), voiced.size());
  CHECK(started.speechStarted);
  CHECK(detector.speechDetected());
}

TEST_CASE("speech over stationary noise endpoints")
{
  EndpointConfig config = baseConfig();
  EndpointDetector detector(config);

  const auto ambient = fanNoise(600, 1200);
  detector.process(ambient.data(), ambient.size());

  const auto voiced = tone(400, 4000);
  const EndpointStatus started = detector.process(voiced.data(), voiced.size());
  CHECK(started.speechStarted);
  CHECK(drainSilence(detector));
  CHECK(detector.speechDetected());
}

TEST_CASE("a short impulse is discarded by the minimum speech")
{
  EndpointConfig config = baseConfig();
  config.startFrames = 1;
  config.endSilenceMs = 30;
  config.minSpeechMs = 300;
  EndpointDetector detector(config);

  const auto click = tone(60, 4000);
  detector.process(click.data(), click.size());
  CHECK_FALSE(drainSilence(detector));
}

TEST_CASE("tts echo-like audio endpoints like speech")
{
  EndpointDetector detector(baseConfig());
  const auto echo = tone(500, 3000, 700.0);
  const EndpointStatus started = detector.process(echo.data(), echo.size());
  CHECK(started.speechStarted);
  CHECK(drainSilence(detector));
}

TEST_CASE("odd-byte and misaligned chunks still frame correctly")
{
  EndpointDetector detector(baseConfig());
  const auto voiced = tone(400, 4000);
  bool started = false;
  std::size_t index = 0;
  const std::size_t step = 7;
  while (index < voiced.size()) {
    const std::size_t take = std::min(step, voiced.size() - index);
    const EndpointStatus status = detector.process(voiced.data() + index, take);
    started = started || status.speechStarted;
    index += take;
  }
  CHECK(started);
  CHECK(drainSilence(detector));
}
