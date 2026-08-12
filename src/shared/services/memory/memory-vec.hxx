#pragma once

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
  std::string enc = "[";
  for (size_t i = 0; i < vec.size(); ++i) {
    if (i > 0)
      enc += ",";
    enc += std::to_string(vec[i]);
  }
  enc += "]";
  return enc;
}

} // namespace memory_vec
