#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

struct ResponseVisitorCropCapabilityDto
{
  std::string token;
  int64_t expiresAt{0};

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponseVisitorCropImageDto
{
  std::string mimeType;
  std::string base64;

  [[nodiscard]] Json::Value toJson() const;
};
