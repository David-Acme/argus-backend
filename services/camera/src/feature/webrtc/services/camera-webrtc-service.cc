#include "camera-webrtc-service.hxx"

#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <shared/services/privacy/camera-audio-policy.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/stream-hub.hxx>

#include <algorithm>
#include <array>
#include <numeric>
#include <ranges>

namespace
{
std::array<std::string, 4> sourcesOf(int64_t cameraId)
{
  const std::string main = Go2rtcManager::sourceName(cameraId, CameraStream::Main);
  const std::string sub = Go2rtcManager::sourceName(cameraId, CameraStream::Sub);
  return {main, sub, Go2rtcManager::opusAudioSource(main), Go2rtcManager::opusAudioSource(sub)};
}

void requireViewerRoom(const WebRtcViewerTally& tally)
{
  const StreamHub::ViewerLimits limits = StreamHub::instance().viewerLimits();
  if (limits.total > 0 && tally.total >= limits.total)
    throw ResponseException(CameraErrors::TooManyViewers);
  if (limits.perCamera > 0 && tally.perCamera >= limits.perCamera)
    throw ResponseException(
        CameraErrors::TooManyViewers.withMessage("too_many_viewers_for_camera"));
}
}

WebRtcViewerTally CameraWebRtcService::tally(const WebRtcViewerCount& count)
{
  const auto consumers = go2rtc_streams::webrtcConsumers(count.streams);
  const auto sources = sourcesOf(count.cameraId);
  const auto watchesCamera = [&sources](const Go2rtcWebRtcConsumer& consumer) {
    return std::ranges::find(sources, consumer.stream) != sources.end();
  };
  const int hubTotal = std::accumulate(count.hubViewers.begin(), count.hubViewers.end(), 0,
                                       [](int sum, const auto& entry) { return sum + entry.second; });
  const auto hubCamera = count.hubViewers.find(count.cameraId);
  return {.perCamera = static_cast<int>(std::ranges::count_if(consumers, watchesCamera)) +
                       (hubCamera == count.hubViewers.end() ? 0 : hubCamera->second),
          .total = static_cast<int>(consumers.size()) + hubTotal};
}

drogon::Task<ResponseCameraWebRtcDto>
CameraWebRtcService::answer(CameraWebRtcRequest request) const
{
  const auto camera = co_await repository_.findById(request.cameraId);
  if (!camera)
    throw ResponseException(CameraErrors::CameraNotFound);
  if (!camera->isEnabled)
    throw ResponseException(CameraErrors::CameraDisabled);

  Go2rtcManager& go2rtc = Go2rtcManager::instance();
  if (!go2rtc.webrtcEnabled() || !go2rtc.isRunning())
    throw ResponseException(CameraErrors::WebRtcUnavailable);

  const bool audio = CameraAudioPolicy::instance().allowed();
  auto offer = webrtc_sdp::prepareOffer({.sdp = request.offer.sdp, .audio = audio});
  if (!offer)
    throw ResponseException(CameraErrors::InvalidWebRtcOffer);

  const auto streams = co_await gateway_.streams();
  if (!streams)
    throw ResponseException(CameraErrors::WebRtcUnavailable);
  const auto hubViewers = StreamHub::instance().viewersByCamera();
  requireViewerRoom(tally({.cameraId = request.cameraId, .streams = *streams, .hubViewers = hubViewers}));

  const CameraStream stream = request.offer.stream();
  const std::string source = Go2rtcManager::sourceName(request.cameraId, stream);
  const std::string analysis = Go2rtcManager::sourceFor(request.cameraId, CameraStreamRole::Analysis);
  const bool transcode = audio && go2rtc_streams::carriesAac(*streams, analysis);
  const auto answer = co_await gateway_.exchange(
      {.source = transcode ? Go2rtcManager::opusAudioSource(source) : source,
       .offer = std::move(*offer),
       .tag = webrtc_viewer::tagOf(request.viewer)});
  if (!answer)
    throw ResponseException(CameraErrors::WebRtcUnavailable);
  co_return ResponseCameraWebRtcDto{
      .sdp = webrtc_sdp::separateAudio(*answer), .stream = stream, .audio = audio};
}
