#pragma once

#include <json/value.h>

struct ResponseHeartbeatDto
{
  Json::Value heartbeat;

  [[nodiscard]] Json::Value toJson() const;
};
