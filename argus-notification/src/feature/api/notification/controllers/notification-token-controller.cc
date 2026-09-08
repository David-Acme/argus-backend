#include "notification-token-controller.hxx"

#include <config/app-config.hxx>
#include <feature/api/notification/dtos/register-notification-token-dto.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/repositories/notification-token/notification-token-query.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
NotificationTokenController::registerToken(drogon::HttpRequestPtr req)
{
  const auto body = RegisterNotificationTokenDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);
  const auto& dev =
      req->getAttributes()->get<DeviceContext>(AppConfig::DEVICE_CTX_KEY);

  co_await service_.registerToken({
      .userId = ctx.sub,
      .deviceHash = dev.deviceHash,
      .token = body.token,
      .platform = body.platform,
      .lang = body.lang});

  Json::Value result;
  result["registered"] = true;
  co_return ApiResponse::ok(result);
}
