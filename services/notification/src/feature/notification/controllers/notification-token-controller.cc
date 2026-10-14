#include "notification-token-controller.hxx"

#include <feature/notification/dtos/register-notification-token-dto.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <http/api-response.hxx>
#include <auth/request-context.hxx>
#include <feature/notification/dtos/unregister-notification-token-dto.hxx>
#include <shared/repositories/notification-token/notification-token-query.hxx>

drogon::Task<drogon::HttpResponsePtr>
NotificationTokenController::registerToken(drogon::HttpRequestPtr req)
{
  const auto body = RegisterNotificationTokenDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);

  co_await service_.registerToken({
      .userId = ctx.sub,
      .deviceHash = dev.deviceHash,
      .token = body.token,
      .platform = body.platform,
      .lang = body.lang,
      .sessionId = ctx.sessionId,
      .provider = body.provider,
      .publicKey = body.publicKey,
      .keyId = body.keyId});

  Json::Value result;
  result["registered"] = true;
  co_return ApiResponse::ok(result);
}

drogon::Task<drogon::HttpResponsePtr>
NotificationTokenController::unregisterToken(drogon::HttpRequestPtr req)
{
  const auto body = UnregisterNotificationTokenDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const bool deleted = co_await service_.unregisterToken({
      .userId = ctx.sub,
      .token = body.token});

  Json::Value result;
  result["deleted"] = deleted;
  co_return ApiResponse::ok(result);
}
