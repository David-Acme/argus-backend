#pragma once

#include <cstdint>
#include <json/value.h>

struct ResponseSirenDto
{
  int64_t cameraId{0};
  bool enabled{false};
  int64_t until{0};

  [[nodiscard]] Json::Value toJson() const;
};
