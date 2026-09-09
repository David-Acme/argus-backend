#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One detected object in frame coordinates (x, y = top-left of the box).
struct DetectedObject
{
  std::string name;
  int cls{-1};
  float confidence{0};
  float x{0};
  float y{0};
  float w{0};
  float h{0};
};

// The only seam of the detection capacity.
class IObjectDetector
{
public:
  virtual ~IObjectDetector() = default;

  virtual bool isLoaded() const = 0;

  // rgb is a tightly packed RGB8 buffer of width*height*3 bytes.
  virtual std::vector<DetectedObject> detect(const uint8_t* rgb, int width,
                                             int height) = 0;

  virtual const std::vector<std::string>& classes() const = 0;
};
