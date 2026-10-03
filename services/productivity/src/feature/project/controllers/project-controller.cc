#include "project-controller.hxx"

#include <errors/response-exception.hxx>
#include <feature/project/dtos/create-project-dto.hxx>
#include <feature/project/dtos/update-project-dto.hxx>
#include <auth/jwt-filter.hxx>
#include <http/api-response.hxx>
#include <productivity/productivity-errors.hxx>
#include <auth/request-context.hxx>

drogon::Task<drogon::HttpResponsePtr>
ProjectController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateProjectDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto row = co_await service_.create(body, ctx.sub);
  co_return ApiResponse::ok(row.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ProjectController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateProjectDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto row = co_await service_.update(
      {.id = id, .body = body, .actorId = ctx.sub});
  if (!row)
    throw ResponseException(ProductivityErrors::ProjectNotFound);
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ProjectController::remove(drogon::HttpRequestPtr req, int64_t id)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  if (!co_await service_.remove(id, ctx.sub))
    throw ResponseException(ProductivityErrors::ProjectNotFound);

  Json::Value result;
  result["deleted"] = true;
  result["id"] = Json::Int64{id};
  co_return ApiResponse::ok(result);
}
