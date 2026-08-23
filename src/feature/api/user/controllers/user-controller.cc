#include "user-controller.hxx"

#include <config/app-config.hxx>
#include <feature/api/user/dtos/update-user-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
UserController::list(drogon::HttpRequestPtr req)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);
  const auto users = co_await service_.list(ctx.sub, ctx.role);
  Json::Value body(Json::arrayValue);
  for (const auto& user : users)
    body.append(user.toJson());
  co_return ApiResponse::ok(body);
}

drogon::Task<drogon::HttpResponsePtr>
UserController::update(drogon::HttpRequestPtr req, int64_t userId)
{
  if (userId <= 0)
    co_return AppConfig::get400Response("Invalid user id");
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);
  const auto body = UpdateUserDto::fromJson(*req->getJsonObject());
  const auto user = co_await service_.update({
      .targetUserId = userId,
      .actorId = ctx.sub,
      .body = body,
  });
  co_return ApiResponse::ok(user.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
UserController::deactivate(drogon::HttpRequestPtr req, int64_t userId)
{
  if (userId <= 0)
    co_return AppConfig::get400Response("Invalid user id");
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);
  co_await service_.deactivate(userId, ctx.sub);
  co_return ApiResponse::noContent();
}
