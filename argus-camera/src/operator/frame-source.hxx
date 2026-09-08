#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A frame captured for analysis: go2rtc JPEG or a lab-injected RGB buffer.
struct CameraFrame
{
  std::vector<uint8_t> jpeg;
  std::vector<uint8_t> rgb;
  int width{0};
  int height{0};
  int64_t capturedAtMs{0};
};

struct FrameGrabRequest
{
  int64_t cameraId{0};
  std::string cameraName;
};

// Frames come from go2rtc, never a device driver.
class IFrameSource
{
public:
  virtual ~IFrameSource() = default;

  virtual drogon::Task<std::optional<CameraFrame>>
  grab(const FrameGrabRequest& request) = 0;
};
