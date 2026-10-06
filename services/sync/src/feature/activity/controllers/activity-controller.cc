#include "activity-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/linked-filter.hxx>
#include <auth/role-filter.hxx>
#include <feature/activity/dtos/list-activity-dto.hxx>
#include <feature/activity/dtos/response-activity-dto.hxx>
#include <http/api-response.hxx>

ActivityController::ActivityController()
{
  auth_filters::requireLinked<DeviceFilter>();
  auth_filters::requireLinked<JwtFilter>();
  auth_filters::requireLinked<RoleFilter>();
}

drogon::Task<drogon::HttpResponsePtr> ActivityController::list(drogon::HttpRequestPtr req)
{
  const auto query = ListActivityDto::fromRequest(req);
  const ResponseActivityDto body{.page = co_await service_.list(query)};
  co_return ApiResponse::ok(body.toJson());
}
