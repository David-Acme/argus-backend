#pragma once

#include <json/value.h>

struct RetainEpisodeDto
{
  bool retain{false};

  static RetainEpisodeDto fromJson(const Json::Value& json);
};
