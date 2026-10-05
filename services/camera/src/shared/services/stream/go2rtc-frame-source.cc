#include <shared/services/stream/go2rtc-frame-source.hxx>

#include <drogon/HttpClient.h>
#include <shared/services/stream/go2rtc-http.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <string>

drogon::Task<std::optional<CameraFrame>>
Go2rtcFrameSource::grab(const FrameGrabRequest& request)
{
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

  auto frameRequest = drogon::HttpRequest::newHttpRequest();
  frameRequest->setMethod(drogon::Get);
  frameRequest->setPath("/api/frame.jpeg");
  frameRequest->setParameter(
      "src", Go2rtcManager::sourceFor(request.cameraId, CameraStreamRole::Analysis));
  if (request.maxAgeMs > 0)
    frameRequest->setParameter("cache", std::to_string(request.maxAgeMs) + "ms");

  const auto client = go2rtc_http::client("frame:" + std::to_string(request.cameraId));
  drogon::HttpResponsePtr response;
  try {
    response = co_await client->sendRequestCoro(frameRequest, 5.0);
  }
  catch (const std::exception&) {
    response.reset();
  }
  if (!response || response->getStatusCode() != drogon::k200OK ||
      response->getBody().empty()) {
    const bool wasOk = [&] {
      std::scoped_lock lock(mutex_);
      const bool ok = lastOkByCamera_[request.cameraId];
      lastOkByCamera_[request.cameraId] = false;
      return ok;
    }();
    if (wasOk)
      LOG_WARN << "Camera operator: frame grab failed for camera "
               << request.cameraId;
    co_return std::nullopt;
  }

  {
    std::scoped_lock lock(mutex_);
    lastOkByCamera_[request.cameraId] = true;
  }

  CameraFrame frame;
  const auto& body = response->getBody();
  frame.jpeg.assign(body.begin(), body.end());
  frame.capturedAtMs = now;
  co_return frame;
}
