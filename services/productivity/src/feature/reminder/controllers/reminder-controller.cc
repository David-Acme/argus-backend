#include "reminder-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <errors/response-exception.hxx>
#include <feature/reminder/dtos/create-reminder-dto.hxx>
#include <feature/reminder/dtos/update-reminder-dto.hxx>
#include <http/api-response.hxx>
#include <productivity/productivity-errors.hxx>
#include <shared/dtos/idempotency-key/idempotency-key-dto.hxx>

drogon::Task<drogon::HttpResponsePtr>
ReminderController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateReminderDto::fromJson(*req->getJsonObject());
  const auto idempotency = IdempotencyKeyDto::fromRequest(req);
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto row = co_await service_.create(
      {.body = body, .userId = ctx.sub, .idempotencyKey = idempotency.key});
  co_return ApiResponse::ok(row.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ReminderController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateReminderDto::fromJson(*req->getJsonObject());
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto row =
      co_await service_.update({.id = id, .body = body, .userId = ctx.sub});
  if (!row)
    throw ResponseException(ProductivityErrors::ReminderNotFound);
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ReminderController::remove(drogon::HttpRequestPtr req, int64_t id)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  if (!co_await service_.remove({.id = id, .userId = ctx.sub}))
    throw ResponseException(ProductivityErrors::ReminderNotFound);
  co_return ApiResponse::noContent();
}
