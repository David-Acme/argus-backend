#include "call-response-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <errors/response-exception.hxx>
#include <feature/call/dtos/response-verdict-dto.hxx>
#include <http/api-response.hxx>
#include <notification/notification-errors.hxx>

#include <utility>

CallResponseController::CallResponseController(std::shared_ptr<CallEngine> engine)
    : service_(std::move(engine))
{
}

drogon::Task<drogon::HttpResponsePtr>
CallResponseController::list(drogon::HttpRequestPtr req)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_return ApiResponse::ok(co_await service_->responses(ctx.sub));
}

drogon::Task<drogon::HttpResponsePtr>
CallResponseController::read(drogon::HttpRequestPtr req, int64_t responseId)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto found =
      co_await service_->response({.userId = ctx.sub, .responseId = responseId});
  if (!found)
    throw ResponseException(NotificationErrors::ResponseNotFound);
  co_return ApiResponse::ok(*found);
}

drogon::Task<drogon::HttpResponsePtr>
CallResponseController::decide(drogon::HttpRequestPtr req, int64_t responseId)
{
  const auto body = ResponseVerdictDto::fromJson(*req->getJsonObject());
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto outcome = co_await service_->verdict(
      {.responseId = responseId,
       .userId = ctx.sub,
       .verdict = responseVerdictFromString(body.verdict).value_or(ResponseVerdict::FalseAlarm)});
  if (outcome.status == ResponseVerdictStatus::NotFound)
    throw ResponseException(NotificationErrors::ResponseNotFound);
  if (outcome.status == ResponseVerdictStatus::Closed)
    throw ResponseException(NotificationErrors::ResponseClosed);
  co_return ApiResponse::ok(outcome.response);
}
