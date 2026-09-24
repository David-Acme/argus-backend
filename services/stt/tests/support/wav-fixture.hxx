#pragma once

#include <cstdint>
#include <cstring>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace
{

std::vector<float> wavSamples(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  const std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  REQUIRE(data.size() > 44);

  const auto chunkAt =
      [&](const char* wanted) -> std::pair<std::string::size_type, uint32_t> {
    std::string::size_type cursor = 12;
    while (cursor + 8 <= data.size()) {
      const std::string id(data.data() + cursor, 4);
      const uint32_t size =
          *reinterpret_cast<const uint32_t*>(data.data() + cursor + 4);
      if (id == wanted)
        return {cursor + 8, size};
      cursor += 8 + size + (size & 1);
    }
    return {std::string::npos, 0};
  };

  const auto [dataStart, dataSize] = chunkAt("data");
  REQUIRE(dataStart != std::string::npos);
  std::vector<float> samples(dataSize / sizeof(int16_t));
  for (size_t i = 0; i < samples.size(); ++i) {
    int16_t raw = 0;
    std::memcpy(&raw, data.data() + dataStart + i * sizeof(int16_t),
                sizeof(raw));
    samples[i] = static_cast<float>(raw) / 32768.0F;
  }
  return samples;
}

}
