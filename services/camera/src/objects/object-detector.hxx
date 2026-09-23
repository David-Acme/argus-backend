#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct DetectedObject
{
  std::string name;
  int cls{-1};
  float confidence{0};
  float x{0};
  float y{0};
  float w{0};
  float h{0};
  int64_t trackId{0};
  int64_t firstSeenMs{0};
};

struct DetectInput
{
  const uint8_t* rgb{nullptr};
  int width{0};
  int height{0};
};

class IObjectDetector
{
public:
  virtual ~IObjectDetector() = default;

  virtual bool isLoaded() const = 0;

  virtual std::vector<DetectedObject> detect(const DetectInput& input) = 0;

  virtual const std::vector<std::string>& classes() const = 0;
};
