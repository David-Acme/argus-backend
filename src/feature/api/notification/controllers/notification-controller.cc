#include "notification-controller.hxx"

#include <config/app-config.hxx>
#include <feature/api/notification/dtos/notification-read-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
NotificationController::markAsRead(drogon::HttpRequestPtr req)
{
  const auto body = NotificationReadDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  co_await service_.markAsRead(ctx.sub, body.ids);

  Json::Value result;
  result["updated"] = true;
  co_return ApiResponse::ok(result);
}
