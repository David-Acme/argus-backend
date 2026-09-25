#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct OperatorZone
{
  int64_t cameraId{0};
  std::string name;
  std::string kind;
  std::vector<std::pair<double, double>> points;
};
