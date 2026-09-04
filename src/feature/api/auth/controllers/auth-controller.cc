#include "auth-controller.hxx"

#include <config/app-config.hxx>
#include <drogon/MultiPart.h>
#include <feature/api/auth/dtos/login-dto.hxx>
#include <feature/api/auth/dtos/refresh-token-dto.hxx>
#include <feature/api/auth/dtos/register-dto.hxx>
#include <feature/api/auth/dtos/response-login-dto.hxx>
#include <feature/api/auth/dtos/response-refresh-token-dto.hxx>
#include <feature/api/auth/dtos/update-me-dto.hxx>
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
AuthController::registerUser(drogon::HttpRequestPtr req)
{
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0)
    co_return AppConfig::get400Response("Invalid multipart form");

  const auto body = RegisterDto::form_multipart(parser);
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AppConfig::DEVICE_CTX_KEY);

  const auto result = co_await service_.registerUser(
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
AuthController::createDeviceLogin(drogon::HttpRequestPtr req)
{
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AppConfig::DEVICE_CTX_KEY);

  const auto result = co_await service_.createDeviceLogin(
      {.deviceHash = dev.deviceHash, .userAgent = dev.userAgent});

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::approveDeviceLogin(drogon::HttpRequestPtr req,
                                   std::string challengeId)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  if (challengeId.empty())
    co_return AppConfig::get400Response("Missing challenge id");

  co_await service_.approveDeviceLogin(challengeId, ctx.sub);

  Json::Value body;
  body["approved"] = true;
  co_return ApiResponse::ok(body);
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::pollDeviceLogin(drogon::HttpRequestPtr,
                                std::string challengeId)
{
  if (challengeId.empty())
    co_return AppConfig::get400Response("Missing challenge id");

  const auto result = co_await service_.pollDeviceLogin(challengeId);

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::refreshToken(drogon::HttpRequestPtr req)
{
  const auto body = RefreshTokenDto::fromJson(*req->getJsonObject());
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AppConfig::DEVICE_CTX_KEY);

  const auto result = co_await service_.refreshToken(body, dev.deviceHash, dev.userAgent);

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

drogon::Task<drogon::HttpResponsePtr>
AuthController::updateMe(drogon::HttpRequestPtr req)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  const auto body = UpdateMeDto::fromJson(*req->getJsonObject());

  co_await service_.updateMe(ctx.sub, body.name);
  co_return ApiResponse::ok();
}
