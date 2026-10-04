#include "call-preference-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <feature/call/dtos/update-call-preference-dto.hxx>
#include <http/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
CallPreferenceController::read(drogon::HttpRequestPtr req)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto preference = co_await service_.read(ctx.sub);
  co_return ApiResponse::ok(preference.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CallPreferenceController::update(drogon::HttpRequestPtr req)
{
  const auto body = UpdateCallPreferenceDto::fromJson(*req->getJsonObject());
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto preference = co_await service_.update(ctx.sub, body);
  co_return ApiResponse::ok(preference.toJson());
}
