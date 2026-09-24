#include "user-controller.hxx"

#include <errors/response-exception.hxx>
#include <feature/user/dtos/update-user-dto.hxx>
#include <auth/jwt-filter.hxx>
#include <http/api-response.hxx>
#include <identity/identity-errors.hxx>
#include <auth/request-context.hxx>

drogon::Task<drogon::HttpResponsePtr>
UserController::list(drogon::HttpRequestPtr req)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
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
    throw ResponseException(IdentityErrors::InvalidUserId);
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
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
    throw ResponseException(IdentityErrors::InvalidUserId);
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_await service_.deactivate(userId, ctx.sub);
  co_return ApiResponse::noContent();
}
