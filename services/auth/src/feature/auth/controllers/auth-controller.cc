#include "auth-controller.hxx"

#include <auth/auth-errors.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <auth/user-role.hxx>
#include <drogon/MultiPart.h>
#include <errors/response-exception.hxx>
#include <feature/auth/dtos/poll-device-login-dto.hxx>
#include <feature/auth/dtos/start-device-login-dto.hxx>
#include <feature/auth/dtos/update-me-dto.hxx>
#include <http/api-response.hxx>
#include <utility>

AuthController::AuthController(const IdentityClient* identity)
    : service_({.jwtService = JwtService{},
                .refreshTokenRepository = RefreshTokenRepository{},
                .deviceCredentialRepository = DeviceCredentialRepository{},
                .challengeRepository = DeviceLoginChallengeRepository{},
                .identity = identity})
{
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::login(drogon::HttpRequestPtr req)
{
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0)
    throw ResponseException(AuthErrors::InvalidMultipartForm);

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
    throw ResponseException(AuthErrors::InvalidMultipartForm);

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
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  Json::Value body(Json::objectValue);
  body["userId"] = static_cast<Json::Int64>(ctx.sub);
  body["name"] = ctx.name;
  body["role"] = userRoleToString(ctx.role);
  body["isActive"] = ctx.isActive;

  co_return ApiResponse::ok(body);
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::createDeviceLogin(drogon::HttpRequestPtr req)
{
  const auto body = StartDeviceLoginDto::fromRequest(req);
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);

  const auto result = co_await service_.createDeviceLogin(
      {.device = {.deviceHash = DeviceFilter::deviceKey(req), .userAgent = dev.userAgent},
       .pollHash = body.pollHash});

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::approveDeviceLogin(drogon::HttpRequestPtr req,
                                   std::string challengeId)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  if (challengeId.empty())
    throw ResponseException(AuthErrors::MissingChallengeId);

  co_await service_.approveDeviceLogin(challengeId, ctx.sub);

  Json::Value body(Json::objectValue);
  body["approved"] = true;
  co_return ApiResponse::ok(body);
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::pollDeviceLogin(drogon::HttpRequestPtr req,
                                std::string challengeId)
{
  if (challengeId.empty())
    throw ResponseException(AuthErrors::MissingChallengeId);

  const auto body = PollDeviceLoginDto::fromRequest(req);
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
  const auto result = co_await service_.pollDeviceLogin(
      {.challengeId = std::move(challengeId),
       .device = {.deviceHash = DeviceFilter::deviceKey(req), .userAgent = dev.userAgent},
       .proof = body.proof});

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
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  co_await service_.logout(
      {.userId = ctx.sub, .name = ctx.name, .role = ctx.role});
  co_return ApiResponse::noContent();
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::updateMe(drogon::HttpRequestPtr req)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto body = UpdateMeDto::fromJson(*req->getJsonObject());

  co_await service_.updateMe({.userId = ctx.sub,
                              .role = userRoleToString(ctx.role),
                              .name = body.name});
  co_return ApiResponse::ok();
}
