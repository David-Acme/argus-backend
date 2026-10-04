#include "auth-controller.hxx"

#include <auth/auth-errors.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <auth/user-role.hxx>
#include <drogon/MultiPart.h>
#include <errors/response-exception.hxx>
#include <feature/auth/dtos/poll-device-login-dto.hxx>
#include <feature/auth/dtos/revoke-sessions-dto.hxx>
#include <feature/auth/infra/client-identity.hxx>
#include <feature/auth/dtos/start-device-login-dto.hxx>
#include <feature/auth/dtos/update-me-dto.hxx>
#include <http/api-response.hxx>
#include <utility>

namespace
{
LoginDeviceInput loginDeviceOf(const drogon::HttpRequestPtr& req)
{
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
  return {.deviceHash = dev.deviceHash,
          .userAgent = dev.userAgent,
          .client = client_identity::of(req)};
}

SessionOwnerInput actorOf(const drogon::HttpRequestPtr& req)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  return {.userId = ctx.sub, .currentSessionId = ctx.sessionId};
}

std::string credentialHashOf(const drogon::HttpRequestPtr& req,
                             const DeviceContext& device)
{
  if (!DeviceFilter::credentialMode() || device.deviceHash.empty())
    return {};
  return DeviceFilter::sha256Hex(
      req->getHeader("X-Argus-Device-Credential"));
}
}

AuthController::AuthController(const IdentityClient* identity,
                               AuthFeatureService::Config config)
    : authService_({.jwtService = JwtService{},
                    .refreshTokenRepository = RefreshTokenRepository{},
                    .deviceCredentialRepository = DeviceCredentialRepository{},
                    .challengeRepository = DeviceLoginChallengeRepository{},
                    .sessions = SessionManagementService(
                        {.refreshTokenRepository = RefreshTokenRepository{}}),
                    .identity = identity},
                   config),
      sessionService_({.refreshTokenRepository = RefreshTokenRepository{}})
{
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::login(drogon::HttpRequestPtr req)
{
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0)
    throw ResponseException(AuthErrors::InvalidMultipartForm);

  auto body = LoginDto::form_multipart(parser);
  const auto result =
      co_await authService_.login(std::move(body), loginDeviceOf(req));

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::registerUser(drogon::HttpRequestPtr req)
{
  drogon::MultiPartParser parser;
  if (parser.parse(req) != 0)
    throw ResponseException(AuthErrors::InvalidMultipartForm);

  auto body = RegisterDto::form_multipart(parser);
  const auto result =
      co_await authService_.registerUser(std::move(body), loginDeviceOf(req));

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

  const auto result = co_await authService_.createDeviceLogin(
      {.device = {.deviceHash = DeviceFilter::deviceKey(req),
                  .userAgent = dev.userAgent,
                  .client = client_identity::of(req)},
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

  co_await authService_.approveDeviceLogin(challengeId, ctx.sub);

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
  const auto result = co_await authService_.pollDeviceLogin(
      {.challengeId = std::move(challengeId),
       .device = {.deviceHash = DeviceFilter::deviceKey(req),
                  .userAgent = dev.userAgent,
                  .client = client_identity::of(req)},
       .proof = body.proof});

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::refreshToken(drogon::HttpRequestPtr req)
{
  const auto body = RefreshTokenDto::fromJson(*req->getJsonObject());
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);

  const auto result = co_await authService_.refreshToken(
      {.body = body,
       .deviceHash = dev.deviceHash,
       .userAgent = dev.userAgent,
       .ip = dev.ip,
       .credentialHash = credentialHashOf(req, dev),
       .client = client_identity::of(req)});

  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::logout(drogon::HttpRequestPtr req)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  co_await authService_.logout({.userId = ctx.sub, .sessionId = ctx.sessionId});
  co_return ApiResponse::noContent();
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::updateMe(drogon::HttpRequestPtr req)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto body = UpdateMeDto::fromJson(*req->getJsonObject());

  co_await authService_.updateMe({.userId = ctx.sub,
                              .role = userRoleToString(ctx.role),
                              .name = body.name});
  co_return ApiResponse::ok();
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::listSessions(drogon::HttpRequestPtr req)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await sessionService_.list(
      {.userId = ctx.sub, .currentSessionId = ctx.sessionId});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::revokeSessions(drogon::HttpRequestPtr req)
{
  const auto body = RevokeSessionsDto::fromRequest(req);
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await sessionService_.revokeScope(
      {.owner = {.userId = ctx.sub, .currentSessionId = ctx.sessionId},
       .scope = body.target});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::revokeSession(drogon::HttpRequestPtr req, std::string sessionId)
{
  const auto& ctx = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto result = co_await sessionService_.revokeOne(
      {.owner = {.userId = ctx.sub, .currentSessionId = ctx.sessionId},
       .sessionId = std::move(sessionId)});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::listEveryUserSessions(drogon::HttpRequestPtr req)
{
  const auto result = co_await sessionService_.listEveryUser(actorOf(req));
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::listUserSessions(drogon::HttpRequestPtr req, int64_t userId)
{
  const auto result = co_await sessionService_.listOfUser(
      {.actor = actorOf(req), .userId = userId});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::revokeUserSessions(drogon::HttpRequestPtr req, int64_t userId)
{
  const auto result = co_await sessionService_.revokeUserSessions(
      {.actor = actorOf(req), .userId = userId});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::revokeUserSession(drogon::HttpRequestPtr req, int64_t userId,
                                  std::string sessionId)
{
  const auto result = co_await sessionService_.revokeUserSession(
      {.actor = actorOf(req), .userId = userId, .sessionId = std::move(sessionId)});
  co_return ApiResponse::ok(result.toJson());
}
