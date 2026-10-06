#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>

namespace wave_writer
{

constexpr int kBitsPerSample = 16;
constexpr float kPeak = 32767.0F;

template <typename Value>
void put(std::ofstream& out, Value value)
{
  const auto bytes = std::bit_cast<std::array<char, sizeof(Value)>>(value);
  out.write(bytes.data(), bytes.size());
}

inline void write(const std::filesystem::path& path, std::span<const float> samples, int sampleRate)
{
  std::ofstream out(path, std::ios::binary);
  const auto dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  out.write("RIFF", 4);
  put<std::uint32_t>(out, 36 + dataBytes);
  out.write("WAVEfmt ", 8);
  put<std::uint32_t>(out, 16);
  put<std::uint16_t>(out, 1);
  put<std::uint16_t>(out, 1);
  put<std::uint32_t>(out, static_cast<std::uint32_t>(sampleRate));
  put<std::uint32_t>(out, static_cast<std::uint32_t>(sampleRate) * (kBitsPerSample / 8));
  put<std::uint16_t>(out, kBitsPerSample / 8);
  put<std::uint16_t>(out, kBitsPerSample);
  out.write("data", 4);
  put<std::uint32_t>(out, dataBytes);
  for (const float sample : samples)
    put<std::int16_t>(out, static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0F, 1.0F) * kPeak)));
}

}
