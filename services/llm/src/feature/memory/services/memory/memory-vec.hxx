#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace memory_vec
{

inline std::string partitionFor(const std::string& scope, int64_t refId)
{
  if (scope == "global")
    return "global";
  return scope + ":" + std::to_string(refId);
}

inline std::string encode(const std::vector<float>& vec)
{
  std::string bytes(vec.size() * sizeof(float), '\0');
  if (!vec.empty())
    std::memcpy(bytes.data(), vec.data(), bytes.size());
  return bytes;
}

inline int64_t factKey(int64_t factId)
{
  return factId;
}

inline int64_t episodeKey(int64_t episodeId)
{
  return -episodeId;
}

inline bool isEpisodeKey(int64_t key)
{
  return key < 0;
}

inline int64_t idOfKey(int64_t key)
{
  return key < 0 ? -key : key;
}

}
