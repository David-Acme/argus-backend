#include "camera-webrtc-service.hxx"

#include <camera/camera-errors.hxx>
#include <config/config-service.hxx>
#include <errors/response-exception.hxx>
#include <shared/services/privacy/camera-audio-policy.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/stream-hub.hxx>

#include <algorithm>
#include <array>
#include <chrono>
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

WebRtcViewerLimits viewerLimits()
{
  constexpr int kDefaultViewersPerUser = 4;
  const StreamHub::ViewerLimits hub = StreamHub::instance().viewerLimits();
  const int perUser = ConfigService::getInt("streaming.max_webrtc_per_user");
  return {.perCamera = hub.perCamera,
          .total = hub.total,
          .perUser = perUser > 0 ? perUser : kDefaultViewersPerUser};
}

void requireSeat(const WebRtcSeatVerdict& verdict)
{
  switch (verdict.refusal) {
    case WebRtcSeatRefusal::None:
      return;
    case WebRtcSeatRefusal::Total:
      throw ResponseException(CameraErrors::TooManyViewers);
    case WebRtcSeatRefusal::Camera:
      throw ResponseException(
          CameraErrors::TooManyViewers.withMessage("too_many_viewers_for_camera"));
    case WebRtcSeatRefusal::User:
      throw ResponseException(
          CameraErrors::TooManyViewers.withMessage("too_many_viewers_for_user"));
  }
}

class SeatHold
{
public:
  SeatHold(WebRtcAdmission& admission, uint64_t ticket) : admission_(admission), ticket_(ticket) {}
  SeatHold(const SeatHold&) = delete;
  SeatHold& operator=(const SeatHold&) = delete;
  ~SeatHold() { admission_.release({.ticket = ticket_, .at = std::chrono::steady_clock::now()}); }

private:
  WebRtcAdmission& admission_;
  uint64_t ticket_;
};
}

WebRtcAdmission& CameraWebRtcService::admission()
{
  static WebRtcAdmission seats;
  return seats;
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

  const auto snapshotAt = std::chrono::steady_clock::now();
  const auto streams = co_await gateway_.streams();
  if (!streams)
    throw ResponseException(CameraErrors::WebRtcUnavailable);
  const auto hubViewers = StreamHub::instance().viewersByCamera();
  const WebRtcViewerTally counted =
      tally({.cameraId = request.cameraId, .streams = *streams, .hubViewers = hubViewers});
  const auto consumers = go2rtc_streams::webrtcConsumers(*streams);
  const WebRtcSeatVerdict seat = admission().reserve({.cameraId = request.cameraId,
                                                      .userId = request.viewer.userId,
                                                      .priority = request.priority,
                                                      .perCamera = counted.perCamera,
                                                      .total = counted.total,
                                                      .consumers = consumers,
                                                      .limits = viewerLimits(),
                                                      .snapshotAt = snapshotAt});
  requireSeat(seat);
  const SeatHold hold(admission(), seat.ticket);

  const CameraStream stream = request.offer.stream();
  const std::string source = Go2rtcManager::sourceName(request.cameraId, stream);
  const std::string analysis = Go2rtcManager::sourceFor(request.cameraId, CameraStreamRole::Analysis);
  const bool transcode = audio && go2rtc_streams::carriesAac(*streams, analysis);
  std::string tag = webrtc_viewer::tagOf(request.viewer);
  const auto answer = co_await gateway_.exchange(
      {.source = transcode ? Go2rtcManager::opusAudioSource(source) : source,
       .offer = std::move(*offer),
       .tag = tag});
  if (!answer)
    throw ResponseException(CameraErrors::WebRtcUnavailable);
  admission().remember({.tag = std::move(tag),
                        .userId = request.viewer.userId,
                        .at = std::chrono::steady_clock::now()});
  const auto hosts = go2rtc.webrtcAnswerHosts();
  co_return ResponseCameraWebRtcDto{
      .sdp = webrtc_sdp::separateAudio(
          webrtc_sdp::screenCandidates({.sdp = *answer, .allowedHosts = hosts})),
      .stream = stream,
      .audio = audio};
}
