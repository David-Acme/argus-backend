#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// FNV-1 over raw bytes: the caption-cache key basis shared by the in-process
// VisionService (scaled pixels + prompt) and the remote adapter (encoded
// JPEG + prompt).
inline uint64_t visionHashBytes(const unsigned char* data, size_t len)
{
  uint64_t h = 14695981039346656037ULL;
  const size_t blocks = len / 8;
  const auto* words = reinterpret_cast<const uint64_t*>(data);
  for (size_t b = 0; b < blocks; ++b) {
    h ^= words[b];
    h *= 1099511628211ULL;
  }
  for (size_t i = blocks * 8; i < len; ++i) {
    h ^= data[i];
    h *= 1099511628211ULL;
  }
  return h;
}

inline uint64_t visionHashBytesAndPrompt(const unsigned char* data, size_t len,
                                         const std::string& prompt)
{
  uint64_t h = visionHashBytes(data, len);
  h ^= visionHashBytes(
      reinterpret_cast<const unsigned char*>(prompt.data()), prompt.size());
  h *= 1099511628211ULL;
  return h;
}