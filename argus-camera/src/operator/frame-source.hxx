#pragma once

#include <drogon/utils/coroutine.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A frame captured for analysis: either the go2rtc frame endpoint's JPEG
// (production) or a decoded RGB buffer injected by the labs and tests.
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

// Frames come from argus-camera's own media surface (go2rtc, owned here since
// F2-2) — never from a device driver (Ruling AB).
class IFrameSource
{
public:
  virtual ~IFrameSource() = default;

  virtual drogon::Task<std::optional<CameraFrame>>
  grab(const FrameGrabRequest& request) = 0;
};