#include "siren-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/linked-filter.hxx>
#include <auth/request-context.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <feature/safety/dtos/response-siren-dto.hxx>
#include <feature/safety/dtos/siren-dto.hxx>
#include <http/api-response.hxx>

#include <utility>

SirenController::SirenController(std::shared_ptr<const SirenService> service)
    : service_(std::move(service))
{
  auth_filters::requireLinked<DeviceFilter>();
  auth_filters::requireLinked<ValidJsonFilter>();
  auth_filters::requireLinked<JwtFilter>();
  auth_filters::requireLinked<RoleFilter>();
}

drogon::Task<drogon::HttpResponsePtr> SirenController::siren(drogon::HttpRequestPtr req)
{
  const auto body = SirenDto::fromJson(*req->getJsonObject());
  const auto& caller =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto& device =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
  const AppSirenResult result = co_await service_->apply(
      {.cameraId = body.cameraId,
       .enabled = body.enabled,
       .actorUserId = caller.sub,
       .actorIp = device.ip});
  co_return ApiResponse::ok(
      ResponseSirenDto{.cameraId = result.cameraId,
                       .enabled = result.enabled,
                       .until = result.until}
          .toJson());
}
