#pragma once

#include <cstdint>
#include <json/value.h>
#include <vector>

struct SplitVisitorDto
{
  std::vector<int64_t> sampleIds;

  static SplitVisitorDto fromJson(const Json::Value& json);
};
