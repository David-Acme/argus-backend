#include "invitation-controller.hxx"

#include <config/app-config.hxx>
#include <feature/api/invitation/dtos/create-invitation-dto.hxx>
#include <feature/api/invitation/dtos/resolve-invitation-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
InvitationController::resolve(drogon::HttpRequestPtr req)
{
  const auto body = ResolveInvitationDto::fromJson(*req->getJsonObject());
  const auto result = co_await service_.resolve(body.token);
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
InvitationController::list(drogon::HttpRequestPtr)
{
  const auto invitations = co_await service_.list();
  Json::Value body(Json::arrayValue);
  for (const auto& invitation : invitations)
    body.append(invitation.toJson());
  co_return ApiResponse::ok(body);
}

drogon::Task<drogon::HttpResponsePtr>
InvitationController::create(drogon::HttpRequestPtr req)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);
  const auto body = CreateInvitationDto::fromJson(*req->getJsonObject());
  const auto result = co_await service_.create(body, ctx.sub);
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
InvitationController::revoke(drogon::HttpRequestPtr req, int64_t invitationId)
{
  if (invitationId <= 0)
    co_return AppConfig::get400Response("Invalid invitation id");
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);
  co_await service_.revoke(invitationId, ctx.sub);
  co_return ApiResponse::noContent();
}
