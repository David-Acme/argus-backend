#include "project-task-controller.hxx"

#include <config/app-config.hxx>
#include <feature/api/project-task/dtos/create-project-task-dto.hxx>
#include <feature/api/project-task/dtos/update-project-task-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
ProjectTaskController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateProjectTaskDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  const auto row = co_await service_.create(body, ctx.sub);
  if (!row)
    co_return AppConfig::get404Response("Project not found");
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ProjectTaskController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateProjectTaskDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  const auto row = co_await service_.update(
      {.id = id, .body = body, .actorId = ctx.sub});
  if (!row)
    co_return AppConfig::get404Response("Task not found");
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ProjectTaskController::remove(drogon::HttpRequestPtr req, int64_t id)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  if (!co_await service_.remove(id, ctx.sub))
    co_return AppConfig::get404Response("Task not found");

  Json::Value result;
  result["deleted"] = true;
  co_return ApiResponse::ok(result);
}
