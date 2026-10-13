#pragma once

#include <json/value.h>

#include <string>

struct ResponseScreenDto
{
  std::string text;

  [[nodiscard]] Json::Value toJson() const;
};
