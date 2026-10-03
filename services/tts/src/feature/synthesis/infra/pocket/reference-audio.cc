#include "reference-audio.hxx"

#include "pcm-rate-converter.hxx"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
constexpr std::uintmax_t kMaxReferenceBytes = std::uintmax_t{32} * 1024 * 1024;
constexpr std::size_t kMaxNameLength = 64;
constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

struct WaveFormat
{
  std::uint16_t format{0};
  std::uint16_t channels{0};
  std::uint32_t rate{0};
  std::uint16_t bits{0};
};

template <typename T>
T readLittle(const std::vector<char>& bytes, std::size_t offset)
{
  if (offset + sizeof(T) > bytes.size())
    throw std::runtime_error("The reference recording is truncated");
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

struct DecodeInput
{
  const std::vector<char>& bytes;
  std::size_t offset{0};
  std::size_t size{0};
  WaveFormat format;
};

std::vector<float> decodeMono(const DecodeInput& input)
{
  const auto& format = input.format;
  const auto sampleBytes = static_cast<std::size_t>(format.bits / 8);
  const auto channels = static_cast<std::size_t>(format.channels);
  const auto frameBytes = sampleBytes * channels;
  if (frameBytes == 0)
    throw std::runtime_error("The reference recording has no audio frames");
  const auto frames = input.size / frameBytes;
  std::vector<float> mono(frames, 0.0F);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    float sum = 0.0F;
    for (std::size_t channel = 0; channel < channels; ++channel) {
      const auto offset = input.offset + (frame * frameBytes) + (channel * sampleBytes);
      if (format.format == kFormatFloat)
        sum += readLittle<float>(input.bytes, offset);
      else
        sum += static_cast<float>(readLittle<std::int16_t>(input.bytes, offset)) / 32768.0F;
    }
    mono[frame] = sum / static_cast<float>(channels);
  }
  return mono;
}
}

bool isReferenceName(std::string_view name)
{
  if (name.size() < 5 || name.size() > kMaxNameLength || name.starts_with('.') || !name.ends_with(".wav") ||
      name.find("..") != std::string_view::npos)
    return false;
  return std::ranges::all_of(name, [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
           value == '.' || value == '_' || value == '-';
  });
}

std::optional<std::filesystem::path> referencePath(const ReferenceAudioInput& input)
{
  if (!isReferenceName(input.name))
    return std::nullopt;
  std::error_code error;
  const auto directory = std::filesystem::canonical(input.directory, error);
  if (error)
    return std::nullopt;
  auto candidate = std::filesystem::canonical(directory / std::filesystem::path(input.name), error);
  if (error || candidate.parent_path() != directory || !std::filesystem::is_regular_file(candidate, error))
    return std::nullopt;
  return candidate;
}

std::vector<float> loadReferenceAudio(const ReferenceAudioInput& input)
{
  const auto path = referencePath(input);
  if (!path.has_value())
    throw std::runtime_error("The reference recording is not a .wav file inside the Pocket references directory");
  if (std::filesystem::file_size(*path) > kMaxReferenceBytes)
    throw std::runtime_error("The reference recording is too large");
  std::ifstream stream(*path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (bytes.size() < 12 || std::string_view(bytes.data(), 4) != "RIFF" || std::string_view(bytes.data() + 8, 4) != "WAVE")
    throw std::runtime_error("The reference recording is not a RIFF/WAVE file");
  std::optional<WaveFormat> format;
  std::size_t position = 12;
  while (position + 8 <= bytes.size()) {
    const std::string_view id(bytes.data() + position, 4);
    const auto size = static_cast<std::size_t>(readLittle<std::uint32_t>(bytes, position + 4));
    const auto body = position + 8;
    if (id == "fmt ") {
      format = WaveFormat{.format = readLittle<std::uint16_t>(bytes, body),
                          .channels = readLittle<std::uint16_t>(bytes, body + 2),
                          .rate = readLittle<std::uint32_t>(bytes, body + 4),
                          .bits = readLittle<std::uint16_t>(bytes, body + 14)};
      if (format->format == kFormatExtensible && size >= 26)
        format->format = readLittle<std::uint16_t>(bytes, body + 24);
    }
    else if (id == "data") {
      if (!format.has_value())
        throw std::runtime_error("The reference recording has no format chunk");
      const bool pcm16 = format->format == kFormatPcm && format->bits == 16;
      const bool float32 = format->format == kFormatFloat && format->bits == 32;
      if ((!pcm16 && !float32) || format->channels == 0 || format->rate == 0)
        throw std::runtime_error("The reference recording must be 16-bit PCM or 32-bit float");
      const auto available = std::min(size, bytes.size() - body);
      const auto mono = decodeMono({.bytes = bytes, .offset = body, .size = available, .format = *format});
      PcmRateConverter converter({.sourceRate = static_cast<int>(format->rate), .targetRate = input.targetRate});
      auto converted = converter.process(mono);
      const auto tail = converter.flush();
      converted.insert(converted.end(), tail.begin(), tail.end());
      return converted;
    }
    position = body + size + (size % 2);
  }
  throw std::runtime_error("The reference recording has no audio data");
}
