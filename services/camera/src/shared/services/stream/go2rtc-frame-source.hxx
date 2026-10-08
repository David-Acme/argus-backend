#pragma once

#include <shared/services/stream/frame-source.hxx>
#include <shared/services/stream/grab-backoff.hxx>

class Go2rtcFrameSource final : public IFrameSource
{
public:
  drogon::Task<std::optional<CameraFrame>>
  grab(const FrameGrabRequest& request) override;

private:
  GrabBackoff backoff_;
};
