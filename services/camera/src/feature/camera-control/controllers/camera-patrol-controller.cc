#include "camera-patrol-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <http/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
CameraPatrolController::read(drogon::HttpRequestPtr, int64_t id)
{
  const auto result = co_await service_.read(id);
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CameraPatrolController::change(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = CameraPatrolDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto& device = req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
  const auto result = co_await service_.change(
      {.cameraId = id, .body = body, .actorUserId = jwt.sub, .actorIp = device.ip});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CameraPatrolController::remove(drogon::HttpRequestPtr req, int64_t id)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto& device = req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
  const auto result = co_await service_.remove(
      {.cameraId = id, .actorUserId = jwt.sub, .actorIp = device.ip});
  co_return ApiResponse::ok(result.toJson());
}
