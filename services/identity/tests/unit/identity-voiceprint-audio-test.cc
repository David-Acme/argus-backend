#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <feature/voiceprint/services/audio/speech-quality.hxx>
#include <feature/voiceprint/services/audio/voice-audio.hxx>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>
#include <numbers>
#include <string>
#include <vector>

namespace
{

struct WavSpec
{
  uint16_t format{1};
  uint16_t channels{1};
  uint32_t sampleRate{16000};
  uint16_t bits{16};
  bool extensible{false};
};

void put16(std::string& out, uint16_t value)
{
  out.push_back(static_cast<char>(value & 0xFFU));
  out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

void put32(std::string& out, uint32_t value)
{
  for (unsigned shift = 0; shift < 32U; shift += 8U)
    out.push_back(static_cast<char>((value >> shift) & 0xFFU));
}

std::string wavOf(const WavSpec& spec, const std::string& payload)
{
  std::string fmt;
  put16(fmt, spec.extensible ? 0xFFFE : spec.format);
  put16(fmt, spec.channels);
  put32(fmt, spec.sampleRate);
  put32(fmt, spec.sampleRate * spec.channels * (spec.bits / 8U));
  put16(fmt, static_cast<uint16_t>(spec.channels * (spec.bits / 8U)));
  put16(fmt, spec.bits);
  if (spec.extensible) {
    put16(fmt, 22);
    put16(fmt, spec.bits);
    put32(fmt, 0);
    put16(fmt, spec.format);
    fmt.append(14, '\0');
  }

  std::string wav = "RIFF";
  put32(wav, static_cast<uint32_t>(4 + 8 + fmt.size() + 8 + payload.size()));
  wav += "WAVE";
  wav += "LIST";
  put32(wav, 4);
  wav += "INFO";
  wav += "fmt ";
  put32(wav, static_cast<uint32_t>(fmt.size()));
  wav += fmt;
  wav += "data";
  put32(wav, static_cast<uint32_t>(payload.size()));
  wav += payload;
  return wav;
}

std::string pcm16(const std::vector<int16_t>& samples)
{
  std::string bytes;
  for (const int16_t sample : samples)
    put16(bytes, std::bit_cast<uint16_t>(sample));
  return bytes;
}

struct Signal
{
  float seconds{0.0F};
  float amplitude{0.0F};
};

std::vector<float> tone(const Signal& signal)
{
  const auto count = static_cast<size_t>(signal.seconds * 16000.0F);
  std::vector<float> samples(count);
  for (size_t index = 0; index < count; ++index)
    samples[index] =
        signal.amplitude * std::sin(2.0F * std::numbers::pi_v<float> * 220.0F *
                                    static_cast<float>(index) / 16000.0F);
  return samples;
}

std::vector<float> noise(const Signal& signal)
{
  uint32_t state = 0x9E3779B9U;
  std::vector<float> samples(static_cast<size_t>(signal.seconds * 16000.0F));
  for (float& sample : samples) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    const float unit = static_cast<float>(state) / 4294967295.0F;
    sample = signal.amplitude * ((2.0F * unit) - 1.0F);
  }
  return samples;
}

std::vector<float> mixed(std::vector<float> base, const std::vector<float>& add)
{
  for (size_t index = 0; index < base.size() && index < add.size(); ++index)
    base[index] += add[index];
  return base;
}

std::vector<float> concat(std::initializer_list<std::vector<float>> parts)
{
  std::vector<float> out;
  for (const auto& part : parts)
    out.insert(out.end(), part.begin(), part.end());
  return out;
}

}

TEST_CASE("a mono 16-bit WAV decodes sample for sample, LIST chunk skipped")
{
  const std::vector<int16_t> samples{0, 1000, -1000, 32767, -32768};
  const auto clip = voice_audio::decodeWav(wavOf({}, pcm16(samples)));
  if (!clip) {
    FAIL("the clip did not decode");
    return;
  }
  CHECK(clip->sampleRate == 16000);
  CHECK(clip->samples == samples);
}

TEST_CASE("stereo, float, 24-bit and extensible WAVs reach one mono clip")
{
  SUBCASE("stereo is averaged")
  {
    const auto clip =
        voice_audio::decodeWav(wavOf({.format = 1,
                                      .channels = 2,
                                      .sampleRate = 44100,
                                      .bits = 16,
                                      .extensible = false},
                                     pcm16({1000, 3000, -2000, -4000})));
    if (!clip) {
      FAIL("the clip did not decode");
      return;
    }
    CHECK(clip->sampleRate == 44100);
    REQUIRE(clip->samples.size() == 2);
    CHECK(std::abs(clip->samples[0] - 2000) <= 1);
    CHECK(std::abs(clip->samples[1] + 3000) <= 1);
  }
  SUBCASE("32-bit float")
  {
    std::string payload;
    for (const float value : {0.5F, -0.25F})
      put32(payload, std::bit_cast<uint32_t>(value));
    const auto clip = voice_audio::decodeWav(wavOf({.format = 3,
                                                    .channels = 1,
                                                    .sampleRate = 48000,
                                                    .bits = 32,
                                                    .extensible = true},
                                                   payload));
    if (!clip) {
      FAIL("the clip did not decode");
      return;
    }
    CHECK(std::abs(clip->samples[0] - 16384) <= 1);
    CHECK(std::abs(clip->samples[1] + 8192) <= 1);
  }
  SUBCASE("24-bit PCM")
  {
    const std::string payload{'\x00', '\x00', '\x40', '\x00', '\x00', '\xC0'};
    const auto clip = voice_audio::decodeWav(wavOf({.format = 1,
                                                    .channels = 1,
                                                    .sampleRate = 16000,
                                                    .bits = 24,
                                                    .extensible = false},
                                                   payload));
    if (!clip) {
      FAIL("the clip did not decode");
      return;
    }
    CHECK(std::abs(clip->samples[0] - 16384) <= 1);
    CHECK(std::abs(clip->samples[1] + 16384) <= 1);
  }
}

TEST_CASE("what is not a usable WAV is refused, not guessed")
{
  CHECK_FALSE(voice_audio::decodeWav("not a wav at all").has_value());
  CHECK_FALSE(voice_audio::decodeWav(wavOf({.format = 1,
                                            .channels = 1,
                                            .sampleRate = 96000,
                                            .bits = 16,
                                            .extensible = false},
                                           pcm16({1, 2, 3})))
                  .has_value());
  CHECK_FALSE(voice_audio::decodeWav(wavOf({.format = 2,
                                            .channels = 1,
                                            .sampleRate = 16000,
                                            .bits = 16,
                                            .extensible = false},
                                           pcm16({1, 2, 3})))
                  .has_value());
  CHECK_FALSE(voice_audio::decodeWav(wavOf({}, "")).has_value());
  const std::vector<int16_t> tooLong(size_t{16000} * 31, 0);
  CHECK_FALSE(voice_audio::decodeWav(wavOf({}, pcm16(tooLong))).has_value());

  std::string truncated = wavOf({}, pcm16({5, 6, 7, 8}));
  truncated.resize(truncated.size() - 4);
  const auto clip = voice_audio::decodeWav(truncated);
  if (!clip) {
    FAIL("the clip did not decode");
    return;
  }
  CHECK(clip->samples == std::vector<int16_t>{5, 6});
}

TEST_CASE("raw PCM16 is little-endian and bounded by rate and length")
{
  const auto clip = voice_audio::decodePcm16(pcm16({0x1234, -2}), 8000);
  if (!clip) {
    FAIL("the clip did not decode");
    return;
  }
  CHECK(clip->samples == std::vector<int16_t>{0x1234, -2});
  CHECK_FALSE(voice_audio::decodePcm16(pcm16({1}), 4000).has_value());
  CHECK_FALSE(voice_audio::decodePcm16("", 16000).has_value());
}

TEST_CASE("every clip reaches the model at 16 kHz in [-1, 1]")
{
  const PcmClip same{.samples = {16384, -16384}, .sampleRate = 16000};
  const auto passthrough = voice_audio::toModelRate(same);
  REQUIRE(passthrough.size() == 2);
  CHECK(passthrough[0] == doctest::Approx(0.5));

  const PcmClip high{.samples = std::vector<int16_t>(48000, 1000),
                     .sampleRate = 48000};
  const auto resampled = voice_audio::toModelRate(high);
  CHECK(resampled.size() >= 15800);
  CHECK(resampled.size() <= 16000);
}

TEST_CASE("speech quality separates speech, silence, noise and clipping")
{
  const SpeechRequirement requirement{
      .minSpeechSeconds = 1.2F,
      .minSnrDb = 12.0F,
      .maxClippedRatio = speech_quality::kMaxClippedRatio};

  SUBCASE("digital silence has no speech")
  {
    const std::vector<float> silence(32000, 0.0F);
    const auto quality = speech_quality::measure(silence);
    CHECK(quality.speechSeconds == doctest::Approx(0.0));
    CHECK(speech_quality::judge(quality, requirement) ==
          SpeechProblem::TooShort);
  }
  SUBCASE("a voiced burst over a quiet floor is measured and trimmed")
  {
    const auto clip = mixed(concat({std::vector<float>(8000, 0.0F),
                                    tone({.seconds = 2.0F, .amplitude = 0.3F}),
                                    std::vector<float>(8000, 0.0F)}),
                            noise({.seconds = 3.0F, .amplitude = 0.001F}));
    const auto quality = speech_quality::measure(clip);
    CHECK(quality.speechSeconds == doctest::Approx(2.0).epsilon(0.05));
    CHECK(quality.snrDb > 30.0F);
    CHECK(speech_quality::judge(quality, requirement) == SpeechProblem::None);
    const auto span = speech_quality::speechSpan(clip, quality);
    CHECK(span.size() < clip.size());
    CHECK(span.size() >= 32000);
  }
  SUBCASE("speech buried in noise is too noisy")
  {
    const auto clip = mixed(tone({.seconds = 3.0F, .amplitude = 0.05F}),
                            noise({.seconds = 3.0F, .amplitude = 0.14F}));
    CHECK(speech_quality::judge(speech_quality::measure(clip), requirement) !=
          SpeechProblem::None);
  }
  SUBCASE("a clipped recording is refused as distorted")
  {
    auto clip = tone({.seconds = 2.0F, .amplitude = 1.0F});
    for (float& sample : clip)
      sample = std::clamp(sample * 3.0F, -1.0F, 1.0F);
    CHECK(speech_quality::judge(speech_quality::measure(clip), requirement) ==
          SpeechProblem::Clipped);
  }
  SUBCASE("a stricter clipping limit refuses what the default lets through")
  {
    auto clip = mixed(concat({std::vector<float>(8000, 0.0F),
                              tone({.seconds = 2.0F, .amplitude = 0.5F}),
                              std::vector<float>(8000, 0.0F)}),
                      noise({.seconds = 3.0F, .amplitude = 0.001F}));
    for (size_t index = 8000; index < 40000; index += 120)
      clip[index] = 1.0F;
    const auto quality = speech_quality::measure(clip);
    REQUIRE(quality.clippedRatio > 0.005F);
    REQUIRE(quality.clippedRatio < speech_quality::kMaxClippedRatio);
    CHECK(speech_quality::judge(quality, requirement) == SpeechProblem::None);
    const SpeechRequirement strict{.minSpeechSeconds = 1.2F,
                                   .minSnrDb = 12.0F,
                                   .maxClippedRatio = 0.005F};
    CHECK(speech_quality::judge(quality, strict) == SpeechProblem::Clipped);
  }
}

TEST_CASE("voice vectors normalize, compare and survive a blob round trip")
{
  const std::vector<float> left{3.0F, 4.0F};
  const auto unit = voice_vector::normalized(left);
  CHECK(unit[0] == doctest::Approx(0.6));
  CHECK(voice_vector::cosine(left, std::vector<float>{6.0F, 8.0F}) ==
        doctest::Approx(1.0));
  CHECK(voice_vector::cosine(left, std::vector<float>{-4.0F, 3.0F}) ==
        doctest::Approx(0.0));
  CHECK(voice_vector::cosine(left, std::vector<float>{1.0F}) == 0.0F);

  const std::vector<std::vector<float>> embeddings{{1.0F, 0.0F}, {0.0F, 1.0F}};
  const auto centroid = voice_vector::centroid(embeddings);
  CHECK(centroid[0] == doctest::Approx(std::sqrt(0.5)));
  CHECK(centroid[1] == doctest::Approx(std::sqrt(0.5)));

  const std::vector<float> values{0.0F, -1.5F, 3.25F, 1e-7F};
  const auto blob = voice_vector::toBlob(values);
  CHECK(blob.size() == values.size() * 4);
  CHECK(voice_vector::fromBlob(std::string_view(blob.data(), blob.size())) ==
        values);
}
