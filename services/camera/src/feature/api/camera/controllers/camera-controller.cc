#include "camera-controller.hxx"

#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <feature/api/camera/dtos/create-camera-dto.hxx>
#include <feature/api/camera/dtos/update-camera-dto.hxx>
#include <http/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
CameraController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateCameraDto::fromJson(*req->getJsonObject());

  const auto row = co_await service_.create(body);
  co_return ApiResponse::ok(row.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CameraController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateCameraDto::fromJson(*req->getJsonObject());

  const auto row = co_await service_.update(id, body);
  if (!row)
    throw ResponseException(CameraErrors::CameraNotFound);
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CameraController::remove(drogon::HttpRequestPtr, int64_t id)
{
  if (!co_await service_.remove(id))
    throw ResponseException(CameraErrors::CameraNotFound);

  Json::Value result;
  result["deleted"] = true;
  co_return ApiResponse::ok(result);
}
