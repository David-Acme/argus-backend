#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

struct ResponseClipContentDto
{
  std::string mimeType;
  std::string base64;
  int64_t durationMs{0};

  Json::Value toJson() const;
};
