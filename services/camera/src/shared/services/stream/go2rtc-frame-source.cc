#include <shared/services/stream/go2rtc-frame-source.hxx>

#include <drogon/HttpClient.h>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>

drogon::Task<std::optional<CameraFrame>>
Go2rtcFrameSource::grab(const FrameGrabRequest& request)
{
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

  auto request2 = drogon::HttpRequest::newHttpRequest();
  request2->setMethod(drogon::Get);
  request2->setPath("/api/frame.jpeg?src=" +
                    Go2rtcManager::sourceFor(request.cameraId, CameraStreamRole::Analysis));

  const auto client = drogon::HttpClient::newHttpClient(
      Go2rtcManager::instance().apiBase());
  drogon::HttpResponsePtr response;
  try {
    response = co_await client->sendRequestCoro(request2, 5.0);
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
