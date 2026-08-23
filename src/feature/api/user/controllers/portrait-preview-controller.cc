#include "portrait-preview-controller.hxx"

#include <config/app-config.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/wrapper/api-response/api-response.hxx>
#include <utility>

drogon::Task<drogon::HttpResponsePtr>
PortraitPreviewController::create(drogon::HttpRequestPtr req,
                                  int64_t portraitUserId)
{
  if (portraitUserId <= 0)
    co_return AppConfig::get400Response("Invalid user id");
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);
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
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);
  const auto result = co_await service_.consume({
      .token = std::move(token),
      .requesterUserId = ctx.sub,
      .requesterRole = ctx.role,
  });
  co_return ApiResponse::ok(result.toJson());
}
