#include "rtc-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/linked-filter.hxx>
#include <auth/request-context.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <http/api-response.hxx>

#include <utility>

RtcController::RtcController(RtcTokenServiceInput input) : service_(std::move(input))
{
  auth_filters::requireLinked<DeviceFilter>();
  auth_filters::requireLinked<ValidJsonFilter>();
  auth_filters::requireLinked<JwtFilter>();
  auth_filters::requireLinked<RoleFilter>();
}

drogon::Task<drogon::HttpResponsePtr> RtcController::token(drogon::HttpRequestPtr req)
{
  const auto body = RtcTokenDto::fromJson(*req->getJsonObject());
  const auto& caller = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await service_.issue(
      {.body = body, .caller = caller, .host = req->getHeader("host")});
  co_return ApiResponse::ok(result.toJson());
}
