#include "project-task-controller.hxx"

#include <errors/response-exception.hxx>
#include <feature/project-task/dtos/create-project-task-dto.hxx>
#include <feature/project-task/dtos/update-project-task-dto.hxx>
#include <shared/dtos/idempotency-key/idempotency-key-dto.hxx>
#include <auth/jwt-filter.hxx>
#include <http/api-response.hxx>
#include <productivity/productivity-errors.hxx>
#include <auth/request-context.hxx>

drogon::Task<drogon::HttpResponsePtr>
ProjectTaskController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateProjectTaskDto::fromJson(*req->getJsonObject());
  const auto idempotency = IdempotencyKeyDto::fromRequest(req);
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto row = co_await service_.create(
      {.body = body, .actorId = ctx.sub, .idempotencyKey = idempotency.key});
  if (!row)
    throw ResponseException(ProductivityErrors::ProjectNotFound);
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ProjectTaskController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateProjectTaskDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto row = co_await service_.update(
      {.id = id, .body = body, .actorId = ctx.sub});
  if (!row)
    throw ResponseException(ProductivityErrors::TaskNotFound);
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ProjectTaskController::remove(drogon::HttpRequestPtr req, int64_t id)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  if (!co_await service_.remove(id, ctx.sub))
    throw ResponseException(ProductivityErrors::TaskNotFound);

  Json::Value result;
  result["deleted"] = true;
  result["id"] = Json::Int64{id};
  co_return ApiResponse::ok(result);
}
