#pragma once

#include <mutex>

namespace ai_init
{

inline std::mutex& llamaMutex()
{
  static std::mutex mutex;
  return mutex;
}

} // namespace ai_init
