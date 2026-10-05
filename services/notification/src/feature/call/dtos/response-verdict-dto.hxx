#pragma once

#include <json/value.h>
#include <string>

struct ResponseVerdictDto
{
  std::string verdict;

  static ResponseVerdictDto fromJson(const Json::Value& json);
};
