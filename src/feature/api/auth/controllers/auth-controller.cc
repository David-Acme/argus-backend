#include "auth-controller.hxx"

#include <config/app-config.hxx>
#include <drogon/MultiPart.h>
#include <feature/api/auth/dtos/login-dto.hxx>
#include <feature/api/auth/dtos/refresh-token-dto.hxx>
#include <feature/api/auth/dtos/response-login-dto.hxx>
#include <feature/api/auth/dtos/response-refresh-token-dto.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
AuthController::login(drogon::HttpRequestPtr req)
{
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0)
    co_return AppConfig::get400Response("Invalid multipart form");

  const auto body = LoginDto::form_multipart(parser);
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AppConfig::DEVICE_CTX_KEY);

  const auto result = co_await service_.login(
      body, {.deviceHash = dev.deviceHash, .userAgent = dev.userAgent});

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::status(drogon::HttpRequestPtr req)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  Json::Value body;
  body["userId"] = ctx.sub;
  body["name"] = ctx.name;
  body["role"] = userRoleToString(ctx.role);
  body["isActive"] = ctx.isActive;

  co_return ApiResponse::ok(body);
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::refreshToken(drogon::HttpRequestPtr req)
{
  const auto body = RefreshTokenDto::fromJson(*req->getJsonObject());
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AppConfig::DEVICE_CTX_KEY);

  const auto result = co_await service_.refreshToken(body, dev.deviceHash);

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::logout(drogon::HttpRequestPtr req)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  co_await service_.logout(ctx.sub);
  co_return ApiResponse::noContent();
}
