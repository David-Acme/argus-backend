#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

struct CreateDeviceLoginDto
{
  std::string challengeId;
  int64_t expiresAt{0};

  Json::Value toJson() const;
};