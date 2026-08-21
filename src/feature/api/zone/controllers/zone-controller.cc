#include "zone-controller.hxx"

#include <config/app-config.hxx>
#include <feature/api/zone/dtos/create-zone-dto.hxx>
#include <feature/api/zone/dtos/update-zone-dto.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
ZoneController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateZoneDto::fromJson(*req->getJsonObject());

  const auto row = co_await service_.create(body);
  if (!row)
    co_return AppConfig::get404Response("Camera not found");
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ZoneController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateZoneDto::fromJson(*req->getJsonObject());

  const auto row = co_await service_.update(id, body);
  if (!row)
    co_return AppConfig::get404Response("Zone not found");
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ZoneController::remove(drogon::HttpRequestPtr, int64_t id)
{
  if (!co_await service_.remove(id))
    co_return AppConfig::get404Response("Zone not found");

  Json::Value result;
  result["deleted"] = true;
  co_return ApiResponse::ok(result);
}
