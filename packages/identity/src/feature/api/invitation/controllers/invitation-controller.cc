#include "invitation-controller.hxx"

#include <errors/response-exception.hxx>
#include <feature/api/invitation/dtos/create-invitation-dto.hxx>
#include <feature/api/invitation/dtos/resolve-invitation-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <http/api-response.hxx>
#include <identity-errors.hxx>
#include <request-context.hxx>

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
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto body = CreateInvitationDto::fromJson(*req->getJsonObject());
  const auto result = co_await service_.create(body, ctx.sub);
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
InvitationController::revoke(drogon::HttpRequestPtr req, int64_t invitationId)
{
  if (invitationId <= 0)
    throw ResponseException(IdentityErrors::InvalidInvitationId);
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  co_await service_.revoke(invitationId, ctx.sub);
  co_return ApiResponse::noContent();
}
