#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

struct ResponsePortraitPreviewCapabilityDto
{
  std::string token;
  int64_t expiresAt{0};

  Json::Value toJson() const;
};
