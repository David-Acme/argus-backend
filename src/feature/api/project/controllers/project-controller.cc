#include "project-controller.hxx"

#include <config/app-config.hxx>
#include <feature/api/project/dtos/create-project-dto.hxx>
#include <feature/api/project/dtos/update-project-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
ProjectController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateProjectDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  const auto row = co_await service_.create(body, ctx.sub);
  co_return ApiResponse::ok(row.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ProjectController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateProjectDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  const auto row = co_await service_.update(id, body, ctx.sub);
  if (!row)
    co_return AppConfig::get404Response("Project not found");
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ProjectController::remove(drogon::HttpRequestPtr req, int64_t id)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  if (!co_await service_.remove(id, ctx.sub))
    co_return AppConfig::get404Response("Project not found");

  Json::Value result;
  result["deleted"] = true;
  co_return ApiResponse::ok(result);
}
