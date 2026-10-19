#include "camera-control-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <feature/camera-control/dtos/camera-aim-dto.hxx>
#include <http/api-response.hxx>
#include <string>
namespace
{
drogon::HttpResponsePtr respond(const CameraControlResult& result)
{
  if (!result)
    throw ResponseException(CameraErrors::CameraNotFound);
  if (!result->ok) {
    if (result->failure == "locked_out")
      throw ResponseException(CameraErrors::CameraControlLocked.withMessage(
          "The camera locked its control login; Argus tries again in " +
          std::to_string(result->retryAfterSeconds) + " s"));
    if (result->failure == "privacy_unsupported")
      throw ResponseException(CameraErrors::CameraPrivacyUnsupported);
    if (result->failure == "privacy_on")
      throw ResponseException(CameraErrors::CameraPrivacyOn);
    if (result->failure == "credential_refused")
      throw ResponseException(result->error.empty()
                                  ? CameraErrors::CameraControlRefused
                                  : CameraErrors::CameraControlRefused.withMessage(
                                        result->error));
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
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_return respond(co_await service_.move(
      {.cameraId = id, .body = body, .actorUserId = jwt.sub, .actorRole = jwt.role}));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::preset(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = CameraPresetDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_return respond(co_await service_.preset(
      {.cameraId = id, .body = body, .actorUserId = jwt.sub, .actorRole = jwt.role}));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::settings(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = CameraSettingsDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto& device =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
  co_return respond(co_await service_.settings(
      {.cameraId = id, .body = body, .actorUserId = jwt.sub,
       .actorIp = device.ip}));
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

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::snapshot(drogon::HttpRequestPtr req, int64_t id)
{
  const auto query = CameraSnapshotDto::fromRequest(req);
  co_return respond(co_await service_.snapshot(id, query.stream()));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::sdDays(drogon::HttpRequestPtr req, int64_t id)
{
  const auto query = CameraSdDaysDto::fromRequest(req);
  co_return respond(co_await service_.sdDays(id, query));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::sdDay(drogon::HttpRequestPtr req, int64_t id)
{
  const auto query = CameraSdDayDto::fromRequest(req);
  co_return respond(co_await service_.sdDay(id, query));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::aim(drogon::HttpRequestPtr req, int64_t id)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_return ApiResponse::ok(co_await service_.aimState({.cameraId = id, .userId = jwt.sub}));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::updateAim(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = CameraAimDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_return ApiResponse::ok(co_await service_.updateAim(
      {.cameraId = id, .patch = body.patch, .actorUserId = jwt.sub}));
}

drogon::Task<drogon::HttpResponsePtr>
CameraControlController::lease(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = CameraLeaseDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_return ApiResponse::ok(co_await service_.lease(
      {.cameraId = id, .actorUserId = jwt.sub, .actorRole = jwt.role, .hold = body.action == "hold"}));
}
