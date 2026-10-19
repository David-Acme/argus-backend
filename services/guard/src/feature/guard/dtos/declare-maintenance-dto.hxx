#pragma once

#include <cstdint>
#include <json/value.h>
#include <vector>

struct DeclareMaintenanceDto
{
  std::vector<int64_t> cameraIds;
  int64_t environmentId{0};
  int64_t minutes{0};
  bool idsTyped{true};

  static DeclareMaintenanceDto fromJson(const Json::Value& json);
};
