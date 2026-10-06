#pragma once

#include <feature/webrtc/infra/go2rtc-webrtc-gateway.hxx>
#include <feature/webrtc/services/webrtc-sdp.hxx>

#include <drogon/utils/coroutine.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class WebRtcSessionCloser
{
public:
  drogon::Task<std::size_t> closeViewer(WebRtcViewer viewer) const;
  drogon::Task<std::size_t> closeAll() const;
  drogon::Task<std::size_t> closeUser(int64_t userId) const;

private:
  drogon::Task<std::size_t> closeWhere(std::function<bool(const std::string&)> matches) const;

  Go2rtcWebRtcGateway gateway_;
};
