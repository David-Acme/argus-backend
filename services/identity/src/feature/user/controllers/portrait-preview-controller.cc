#include "portrait-preview-controller.hxx"

#include <errors/response-exception.hxx>
#include <auth/jwt-filter.hxx>
#include <http/api-response.hxx>
#include <identity/identity-errors.hxx>
#include <auth/request-context.hxx>
#include <utility>

drogon::Task<drogon::HttpResponsePtr>
PortraitPreviewController::create(drogon::HttpRequestPtr req,
                                  int64_t portraitUserId)
{
  if (portraitUserId <= 0)
    throw ResponseException(IdentityErrors::InvalidUserId);
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await service_.create({
      .portraitUserId = portraitUserId,
      .requesterUserId = ctx.sub,
      .requesterRole = ctx.role,
  });
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
PortraitPreviewController::consume(drogon::HttpRequestPtr req,
                                   std::string token)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await service_.consume({
      .token = std::move(token),
      .requesterUserId = ctx.sub,
      .requesterRole = ctx.role,
  });
  co_return ApiResponse::ok(result.toJson());
}
