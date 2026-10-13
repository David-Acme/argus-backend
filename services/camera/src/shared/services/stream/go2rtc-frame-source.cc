#include <shared/services/stream/go2rtc-frame-source.hxx>

#include <drogon/HttpClient.h>
#include <shared/services/stream/go2rtc-http.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <string>

namespace
{
constexpr double kAnalysisGrabTimeoutS = 5.0;
constexpr double kFullGrabTimeoutS = 8.0;
}

drogon::Task<std::optional<CameraFrame>>
Go2rtcFrameSource::grab(const FrameGrabRequest& request)
{
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const bool paced = camera_stream_role::isWarm(request.stream);
  const GrabPoint point{.cameraId = request.cameraId, .nowMs = now};
  const GrabAttempt attempt =
      paced ? backoff_.begin(point) : GrabAttempt{.attempt = true, .firstFailure = false, .retryDelayMs = 0};
  if (!attempt.attempt)
    co_return std::nullopt;

  auto frameRequest = drogon::HttpRequest::newHttpRequest();
  frameRequest->setMethod(drogon::Get);
  frameRequest->setPath("/api/frame.jpeg");
  frameRequest->setParameter("src", Go2rtcManager::sourceName(request.cameraId, request.stream));
  if (request.maxAgeMs > 0)
    frameRequest->setParameter("cache", std::to_string(request.maxAgeMs) + "ms");

  const auto client = go2rtc_http::client("frame:" + std::to_string(request.cameraId) + ":" +
                                          cameraStreamToString(request.stream));
  drogon::HttpResponsePtr response;
  try {
    response = co_await client->sendRequestCoro(frameRequest, paced ? kAnalysisGrabTimeoutS : kFullGrabTimeoutS);
  }
  catch (const std::exception&) {
    response.reset();
  }
  if (!response || response->getStatusCode() != drogon::k200OK ||
      response->getBody().empty()) {
    if (!paced)
      co_return std::nullopt;
    backoff_.noteFailure(point);
    if (attempt.firstFailure)
      LOG_WARN << "Camera operator: frame grab failed for camera "
               << request.cameraId << "; retrying in "
               << attempt.retryDelayMs / 1000 << " s";
    co_return std::nullopt;
  }
  if (paced)
    backoff_.noteSuccess(request.cameraId);

  CameraFrame frame;
  const auto& body = response->getBody();
  frame.jpeg.assign(body.begin(), body.end());
  frame.capturedAtMs = now;
  co_return frame;
}
