#include "camera-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <feature/camera/dtos/create-camera-dto.hxx>
#include <feature/camera/dtos/update-camera-dto.hxx>
#include <feature/camera/dtos/probe-camera-dto.hxx>
#include <http/api-response.hxx>
#include <shared/services/camera-catalog/camera-catalog.hxx>

drogon::Task<drogon::HttpResponsePtr>
CameraController::catalog(drogon::HttpRequestPtr)
{
  co_return ApiResponse::ok(camera_catalog::toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CameraController::overview(drogon::HttpRequestPtr req)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_return ApiResponse::ok(co_await overviewService_.overview(jwt.role));
}

drogon::Task<drogon::HttpResponsePtr>
CameraController::probe(drogon::HttpRequestPtr req)
{
  const auto body = ProbeCameraDto::fromJson(*req->getJsonObject());
  co_return ApiResponse::ok(co_await probeService_.probe(body));
}

drogon::Task<drogon::HttpResponsePtr>
CameraController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateCameraDto::fromJson(*req->getJsonObject());

  const auto row = co_await cameraService_.create(body);
  co_return ApiResponse::ok(row.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CameraController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateCameraDto::fromJson(*req->getJsonObject());

  const auto row = co_await cameraService_.update(id, body);
  if (!row)
    throw ResponseException(CameraErrors::CameraNotFound);
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CameraController::remove(drogon::HttpRequestPtr, int64_t id)
{
  if (!co_await cameraService_.remove(id))
    throw ResponseException(CameraErrors::CameraNotFound);

  Json::Value result;
  result["deleted"] = true;
  co_return ApiResponse::ok(result);
}
