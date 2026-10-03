#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

inline uint64_t visionHashBytes(const unsigned char* data, size_t len)
{
  uint64_t h = 14695981039346656037ULL;
  const size_t blocks = len / 8;
  for (size_t b = 0; b < blocks; ++b) {
    uint64_t word = 0;
    std::memcpy(&word, data + b * 8, sizeof(word));
    h ^= word;
    h *= 1099511628211ULL;
  }
  for (size_t i = blocks * 8; i < len; ++i) {
    h ^= data[i];
    h *= 1099511628211ULL;
  }
  return h;
}

struct VisionHashBytesAndPromptInput
{
  const unsigned char* data;
  size_t len{0};
  const std::string& prompt;
};

inline uint64_t visionHashBytesAndPrompt(
    const VisionHashBytesAndPromptInput& input)
{
  uint64_t h = visionHashBytes(input.data, input.len);
  h ^= visionHashBytes(
      reinterpret_cast<const unsigned char*>(input.prompt.data()),
      input.prompt.size());
  h *= 1099511628211ULL;
  return h;
}
