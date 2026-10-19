#pragma once

#include <json/value.h>

#include <cstdint>

struct ResponseRtcIceDto
{
  Json::Value iceServers{Json::arrayValue};
  int64_t expiresAt{0};

  [[nodiscard]] Json::Value toJson() const;
};
