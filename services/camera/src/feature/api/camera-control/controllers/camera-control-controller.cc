#include "camera-control-controller.hxx"

#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <http/api-response.hxx>
namespace
{
drogon::HttpResponsePtr respond(const CameraControlResult& result)
{
  if (!result)
    throw ResponseException(CameraErrors::CameraNotFound);
  if (!result->ok) {
    throw ResponseException(result->error.empty()
                                ? CameraErrors::CameraUnreachable
                                : CameraErrors::CameraUnreachable.withMessage(
                                      result->error));
  }
  return ApiResponse::ok(result->data);
}
}

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
