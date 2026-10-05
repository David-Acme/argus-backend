#include "webrtc-session-closer.hxx"

#include <runtime/blocking-task.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <utility>

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
  const auto streams = co_await gateway_.streams();
  if (!streams)
    co_return 0;
  const auto consumers = go2rtc_streams::webrtcConsumers(*streams);
  const auto open = static_cast<std::size_t>(std::ranges::count_if(
      consumers, [&matches](const Go2rtcWebRtcConsumer& consumer) { return matches(consumer.userAgent); }));
  if (open == 0)
    co_return 0;
  const bool restarted =
      co_await BlockingTask<bool>([] { return Go2rtcManager::instance().restart(); });
  LOG_INFO << "Camera WebRTC: closed " << open << " live view(s) by restarting go2rtc"
           << (restarted ? "" : " (restart failed)");
  co_return open;
}
