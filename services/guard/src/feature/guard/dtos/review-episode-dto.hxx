#pragma once

#include <json/value.h>
#include <string>

struct ReviewEpisodeDto
{
  std::string label;

  static ReviewEpisodeDto fromJson(const Json::Value& json);
};
