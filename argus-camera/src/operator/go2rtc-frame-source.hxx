#pragma once

#include <operator/frame-source.hxx>

#include <mutex>
#include <string>
#include <unordered_map>

class Go2rtcFrameSource final : public IFrameSource
{
public:
  drogon::Task<std::optional<CameraFrame>>
  grab(const FrameGrabRequest& request) override;

private:
  std::mutex mutex_;
  std::unordered_map<int64_t, bool> lastOkByCamera_;
};