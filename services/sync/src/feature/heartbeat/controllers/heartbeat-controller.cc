#include "heartbeat-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/linked-filter.hxx>
#include <auth/request-context.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <feature/heartbeat/dtos/response-heartbeat-dto.hxx>
#include <http/api-response.hxx>

#include <utility>

HeartbeatController::HeartbeatController(
    std::shared_ptr<const HeartbeatService> service)
    : service_(std::move(service))
{
  auth_filters::requireLinked<DeviceFilter>();
  auth_filters::requireLinked<ValidJsonFilter>();
  auth_filters::requireLinked<JwtFilter>();
  auth_filters::requireLinked<RoleFilter>();
}

drogon::Task<drogon::HttpResponsePtr>
HeartbeatController::heartbeat(drogon::HttpRequestPtr req)
{
  const auto& caller =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const ResponseHeartbeatDto result{.heartbeat = service_->heartbeatFor(caller.sub)};
  co_return ApiResponse::ok(result.toJson());
}
