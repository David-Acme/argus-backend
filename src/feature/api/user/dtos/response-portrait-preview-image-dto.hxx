#pragma once

#include <json/value.h>
#include <string>

struct ResponsePortraitPreviewImageDto
{
  std::string mimeType;
  std::string base64;

  Json::Value toJson() const;
};
