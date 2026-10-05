#include "webrtc-session-closer.hxx"

#include <shared/services/stream/go2rtc-manager.hxx>

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace
{
constexpr int kStreamsAttempts = 3;
constexpr auto kStreamsRetry = std::chrono::seconds(1);
}

drogon::Task<std::size_t> WebRtcSessionCloser::closeViewer(WebRtcViewer viewer) const
{
  std::string tag = webrtc_viewer::tagOf(viewer);
  co_return co_await closeWhere(
      [tag = std::move(tag)](const std::string& userAgent) { return userAgent == tag; });
}

drogon::Task<std::size_t> WebRtcSessionCloser::closeAll() const
{
  co_return co_await closeWhere(
      [](const std::string& userAgent) { return webrtc_viewer::isTagged(userAgent); });
}

drogon::Task<std::size_t>
WebRtcSessionCloser::closeWhere(std::function<bool(const std::string&)> matches) const
{
  std::optional<Json::Value> streams;
  for (int attempt = 0; attempt < kStreamsAttempts && !streams; ++attempt) {
    if (attempt > 0)
      co_await drogon::sleepCoro(drogon::app().getLoop(), kStreamsRetry);
    streams = co_await gateway_.streams();
  }
  if (!streams) {
    if (!Go2rtcManager::instance().isRunning())
      co_return 0;
    Go2rtcManager::instance().requestRestart();
    LOG_WARN << "Camera WebRTC: go2rtc did not list its viewers; restarting it so no "
                "closed session keeps watching";
    co_return 0;
  }
  const auto consumers = go2rtc_streams::webrtcConsumers(*streams);
  const auto open = static_cast<std::size_t>(std::ranges::count_if(
      consumers, [&matches](const Go2rtcWebRtcConsumer& consumer) { return matches(consumer.userAgent); }));
  if (open == 0)
    co_return 0;
  Go2rtcManager::instance().requestRestart();
  LOG_INFO << "Camera WebRTC: closing " << open << " live view(s) by restarting go2rtc";
  co_return open;
}
