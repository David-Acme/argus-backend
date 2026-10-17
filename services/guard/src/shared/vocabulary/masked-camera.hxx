#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct MaskedCamera
{
  int64_t cameraId{0};
  std::string name{};
};

using MaskedCameraDirectory = std::function<std::vector<MaskedCamera>()>;
