#include "auth-controller.hxx"

#include <drogon/MultiPart.h>
#include <errors/response-exception.hxx>
#include <feature/api/auth/dtos/login-dto.hxx>
#include <feature/api/auth/dtos/refresh-token-dto.hxx>
#include <feature/api/auth/dtos/register-dto.hxx>
#include <feature/api/auth/dtos/response-login-dto.hxx>
#include <feature/api/auth/dtos/response-refresh-token-dto.hxx>
#include <feature/api/auth/dtos/update-me-dto.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <http/api-response.hxx>
#include <identity/identity-errors.hxx>
#include <auth/request-context.hxx>

drogon::Task<drogon::HttpResponsePtr>
AuthController::login(drogon::HttpRequestPtr req)
{
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0)
    throw ResponseException(IdentityErrors::InvalidMultipartForm);

  const auto body = LoginDto::form_multipart(parser);
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);

  const auto result = co_await service_.login(
      body, {.deviceHash = dev.deviceHash, .userAgent = dev.userAgent});

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::registerUser(drogon::HttpRequestPtr req)
{
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0)
    throw ResponseException(IdentityErrors::InvalidMultipartForm);

  const auto body = RegisterDto::form_multipart(parser);
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);

  const auto result = co_await service_.registerUser(
      body, {.deviceHash = dev.deviceHash, .userAgent = dev.userAgent});

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::status(drogon::HttpRequestPtr req)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

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
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);

  const auto result = co_await service_.createDeviceLogin(
      {.deviceHash = dev.deviceHash, .userAgent = dev.userAgent});

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::approveDeviceLogin(drogon::HttpRequestPtr req,
                                   std::string challengeId)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  if (challengeId.empty())
    throw ResponseException(IdentityErrors::MissingChallengeId);

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
    throw ResponseException(IdentityErrors::MissingChallengeId);

  const auto result = co_await service_.pollDeviceLogin(challengeId);

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::refreshToken(drogon::HttpRequestPtr req)
{
  const auto body = RefreshTokenDto::fromJson(*req->getJsonObject());
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);

  const auto result = co_await service_.refreshToken(
      {.body = body, .deviceHash = dev.deviceHash, .userAgent = dev.userAgent});

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::logout(drogon::HttpRequestPtr req)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  co_await service_.logout(ctx.sub);
  co_return ApiResponse::noContent();
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::updateMe(drogon::HttpRequestPtr req)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto body = UpdateMeDto::fromJson(*req->getJsonObject());

  co_await service_.updateMe(ctx.sub, body.name);
  co_return ApiResponse::ok();
}
