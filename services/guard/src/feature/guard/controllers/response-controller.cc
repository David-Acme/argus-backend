#include "response-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <feature/guard/dtos/update-duty-dto.hxx>
#include <feature/guard/dtos/update-response-dto.hxx>
#include <http/api-response.hxx>
#include <utility>

ResponseController::ResponseController(ResponseFeatureDependencies dependencies)
    : service_(std::move(dependencies))
{
}

drogon::Task<drogon::HttpResponsePtr>
ResponseController::view(drogon::HttpRequestPtr req, int64_t environmentId)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_return ApiResponse::ok(co_await service_.view(
      {.environmentId = environmentId, .userId = jwt.sub, .role = jwt.role}));
}

drogon::Task<drogon::HttpResponsePtr>
ResponseController::replace(drogon::HttpRequestPtr req, int64_t environmentId)
{
  const auto body = UpdateResponseDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_return ApiResponse::ok(co_await service_.replace(
      {.environmentId = environmentId, .userId = jwt.sub, .body = body}));
}

drogon::Task<drogon::HttpResponsePtr>
ResponseController::duty(drogon::HttpRequestPtr req, int64_t environmentId)
{
  const auto body = UpdateDutyDto::fromJson(*req->getJsonObject());
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_return ApiResponse::ok(
      co_await service_.setDuty({.environmentId = environmentId,
                                 .userId = jwt.sub,
                                 .role = jwt.role,
                                 .onDuty = body.onDuty}));
}
