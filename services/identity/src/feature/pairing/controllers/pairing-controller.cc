#include "pairing-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/request-context.hxx>
#include <feature/pairing/dtos/pairing-dto.hxx>
#include <http/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
PairingController::pair(drogon::HttpRequestPtr req)
{
  const auto body = PairingDto::fromJson(*req->getJsonObject());
  const auto& device =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
  const auto result =
      service_.pair({.code = body.code, .deviceHash = device.deviceHash});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
PairingController::status(drogon::HttpRequestPtr)
{
  const auto result = co_await service_.status();
  co_return ApiResponse::ok(result.toJson());
}
