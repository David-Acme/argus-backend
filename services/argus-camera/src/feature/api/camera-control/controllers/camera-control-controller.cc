#include "camera-control-controller.hxx"

#include <config/app-config.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

namespace
{
// A device that answers "no" is 502, not a server error; a missing row is 404.
drogon::HttpResponsePtr respond(const CameraControlResult& result)
{
  if (!result)
    return AppConfig::get404Response("Camera not found");
  if (!result->ok)
    return AppConfig::get502Response(result->error.empty()
                                         ? "The camera refused the command"
                                         : result->error,
                                     "CAMERA_UNREACHABLE");
  return ApiResponse::ok(result->data);
}
} // namespace

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::status(drogon::HttpRequestPtr, int64_t id)
{
  co_return respond(co_await service_.status(id));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::presets(drogon::HttpRequestPtr, int64_t id)
{
  co_return respond(co_await service_.presets(id));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::move(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = CameraPtzDto::fromJson(*req->getJsonObject());
  co_return respond(co_await service_.move(id, body));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::preset(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = CameraPresetDto::fromJson(*req->getJsonObject());
  co_return respond(co_await service_.preset(id, body));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::settings(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = CameraSettingsDto::fromJson(*req->getJsonObject());
  co_return respond(co_await service_.settings(id, body));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::capabilities(drogon::HttpRequestPtr, int64_t id)
{
  co_return respond(co_await service_.capabilities(id));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::talk(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = CameraTalkDto::fromJson(*req->getJsonObject());
  co_return respond(co_await service_.speak(id, body));
}
