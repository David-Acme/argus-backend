#pragma once

#include <cstdint>
#include <json/value.h>

class HeartbeatSource
{
public:
  virtual ~HeartbeatSource() = default;

  [[nodiscard]] virtual Json::Value heartbeatFor(int64_t userId) const = 0;
};
