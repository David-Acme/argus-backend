#include "voice-audio.hxx"

#include <algorithm>
#include <array>
#include <audio/audio-resampler.hxx>
#include <bit>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <span>

namespace
{

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;
constexpr size_t kRiffHeaderBytes = 12;
constexpr size_t kChunkHeaderBytes = 8;
constexpr size_t kFmtMinBytes = 16;
constexpr size_t kExtensibleMinBytes = 26;
constexpr uint16_t kMaxChannels = 8;
constexpr float kInt16Scale = 32768.0F;

struct WavFormat
{
  uint16_t format{0};
  uint16_t channels{0};
  uint32_t sampleRate{0};
  uint16_t bitsPerSample{0};
};

uint8_t byteAt(std::string_view bytes, size_t offset)
{
  return static_cast<uint8_t>(bytes[offset]);
}

uint16_t readU16(std::string_view bytes, size_t offset)
{
  return static_cast<uint16_t>(byteAt(bytes, offset) |
                               (byteAt(bytes, offset + 1) << 8U));
}

uint32_t readU32(std::string_view bytes, size_t offset)
{
  return static_cast<uint32_t>(byteAt(bytes, offset)) |
         (static_cast<uint32_t>(byteAt(bytes, offset + 1)) << 8U) |
         (static_cast<uint32_t>(byteAt(bytes, offset + 2)) << 16U) |
         (static_cast<uint32_t>(byteAt(bytes, offset + 3)) << 24U);
}

bool rateSupported(int rate)
{
  return rate >= voice_audio::kMinRate && rate <= voice_audio::kMaxRate;
}

std::optional<WavFormat> parseFormat(std::string_view chunk)
{
  if (chunk.size() < kFmtMinBytes)
    return std::nullopt;
  WavFormat format{.format = readU16(chunk, 0),
                   .channels = readU16(chunk, 2),
                   .sampleRate = readU32(chunk, 4),
                   .bitsPerSample = readU16(chunk, 14)};
  if (format.format == kFormatExtensible) {
    if (chunk.size() < kExtensibleMinBytes)
      return std::nullopt;
    format.format = readU16(chunk, 24);
  }
  return format;
}

struct SampleCursor
{
  const WavFormat& format;
  std::string_view data;
  size_t offset{0};
};

float decodeSample(const SampleCursor& cursor)
{
  const WavFormat& format = cursor.format;
  const std::string_view frame = cursor.data;
  const size_t offset = cursor.offset;
  switch (format.bitsPerSample) {
    case 8:
      return (static_cast<float>(byteAt(frame, offset)) - 128.0F) / 128.0F;
    case 16:
      return static_cast<float>(
                 std::bit_cast<int16_t>(readU16(frame, offset))) /
             kInt16Scale;
    case 24: {
      const uint32_t raw =
          static_cast<uint32_t>(byteAt(frame, offset)) |
          (static_cast<uint32_t>(byteAt(frame, offset + 1)) << 8U) |
          (static_cast<uint32_t>(byteAt(frame, offset + 2)) << 16U);
      const uint32_t extended =
          (raw & 0x800000U) != 0U ? raw | 0xFF000000U : raw;
      return static_cast<float>(std::bit_cast<int32_t>(extended)) / 8388608.0F;
    }
    default:
      break;
  }
  const uint32_t word = readU32(frame, offset);
  if (format.format == kFormatFloat)
    return std::bit_cast<float>(word);
  return static_cast<float>(static_cast<double>(std::bit_cast<int32_t>(word)) /
                            2147483648.0);
}

bool formatSupported(const WavFormat& format)
{
  if (format.channels == 0 || format.channels > kMaxChannels ||
      !rateSupported(static_cast<int>(format.sampleRate)))
    return false;
  if (format.format == kFormatFloat)
    return format.bitsPerSample == 32;
  if (format.format != kFormatPcm)
    return false;
  constexpr std::array<uint16_t, 4> kDepths{8, 16, 24, 32};
  return std::ranges::find(kDepths, format.bitsPerSample) != kDepths.end();
}

int16_t toInt16(float value)
{
  const float scaled = std::round(value * kInt16Scale);
  return static_cast<int16_t>(std::clamp(scaled, -32768.0F, 32767.0F));
}

std::optional<PcmClip> downmix(const WavFormat& format, std::string_view data)
{
  const size_t sampleBytes = format.bitsPerSample / 8U;
  const size_t frameBytes = sampleBytes * format.channels;
  const size_t frames = data.size() / frameBytes;
  if (frames == 0 ||
      frames >
          static_cast<size_t>(voice_audio::kMaxClipSeconds) * format.sampleRate)
    return std::nullopt;

  PcmClip clip{.samples = {},
               .sampleRate = static_cast<int>(format.sampleRate)};
  clip.samples.reserve(frames);
  for (size_t frame = 0; frame < frames; ++frame) {
    float sum = 0.0F;
    for (size_t channel = 0; channel < format.channels; ++channel)
      sum += decodeSample(
          {.format = format,
           .data = data,
           .offset = (frame * frameBytes) + (channel * sampleBytes)});
    clip.samples.push_back(toInt16(sum / static_cast<float>(format.channels)));
  }
  return clip;
}

}

namespace voice_audio
{

std::optional<PcmClip> decodeWav(std::string_view bytes)
{
  if (bytes.size() < kRiffHeaderBytes || !bytes.starts_with("RIFF") ||
      bytes.substr(8, 4) != "WAVE")
    return std::nullopt;

  std::optional<WavFormat> format;
  size_t offset = kRiffHeaderBytes;
  while (offset + kChunkHeaderBytes <= bytes.size()) {
    const std::string_view id = bytes.substr(offset, 4);
    const size_t declared = readU32(bytes, offset + 4);
    const size_t start = offset + kChunkHeaderBytes;
    const size_t length = std::min(declared, bytes.size() - start);
    const std::string_view chunk = bytes.substr(start, length);
    if (id == "fmt ") {
      format = parseFormat(chunk);
      if (!format || !formatSupported(*format))
        return std::nullopt;
    }
    else if (id == "data") {
      if (!format)
        return std::nullopt;
      return downmix(*format, chunk);
    }
    offset = start + length + (length % 2);
  }
  return std::nullopt;
}

std::optional<PcmClip> decodePcm16(std::string_view bytes, int sampleRate)
{
  const size_t count = bytes.size() / 2;
  if (count == 0 || !rateSupported(sampleRate) ||
      count > static_cast<size_t>(kMaxClipSeconds) *
                  static_cast<size_t>(sampleRate))
    return std::nullopt;
  PcmClip clip{.samples = {}, .sampleRate = sampleRate};
  clip.samples.reserve(count);
  for (size_t index = 0; index < count; ++index)
    clip.samples.push_back(std::bit_cast<int16_t>(readU16(bytes, index * 2)));
  return clip;
}

std::optional<PcmClip> decode(const EncodedVoice& voice)
{
  if (voice.encoding == VoiceEncoding::Pcm16)
    return decodePcm16(voice.bytes, voice.sampleRate);
  return decodeWav(voice.bytes);
}

std::vector<float> toModelRate(const PcmClip& clip)
{
  std::vector<int16_t> resampled;
  std::span<const int16_t> source = clip.samples;
  if (clip.sampleRate != kModelRate) {
    AudioResampler resampler(
        {.sourceRate = clip.sampleRate, .targetRate = kModelRate});
    resampled = resampler.process(clip.samples.data(), clip.samples.size());
    source = resampled;
  }
  std::vector<float> samples;
  samples.reserve(source.size());
  std::ranges::transform(source, std::back_inserter(samples),
                         [](int16_t value) {
                           return static_cast<float>(value) / kInt16Scale;
                         });
  return samples;
}

}
