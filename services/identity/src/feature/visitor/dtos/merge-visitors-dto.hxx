#pragma once

#include <cstdint>
#include <json/value.h>
#include <vector>

struct MergeVisitorsDto
{
  std::vector<int64_t> sourceIds;

  static MergeVisitorsDto fromJson(const Json::Value& json);
};
