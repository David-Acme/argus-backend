#include "notification-controller.hxx"

#include <feature/api/notification/dtos/delivery-summary-dto.hxx>
#include <feature/api/notification/dtos/notification-ack-dto.hxx>
#include <feature/api/notification/dtos/notification-read-dto.hxx>
#include <auth/jwt-filter.hxx>
#include <http/api-response.hxx>
#include <request-context.hxx>

drogon::Task<drogon::HttpResponsePtr>
NotificationController::markAsRead(drogon::HttpRequestPtr req)
{
  const auto body = NotificationReadDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  co_await service_.markAsRead(ctx.sub, body.ids);

  Json::Value result;
  result["updated"] = true;
  co_return ApiResponse::ok(result);
}

drogon::Task<drogon::HttpResponsePtr>
NotificationController::ack(drogon::HttpRequestPtr req)
{
  const auto body = NotificationAckDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  Json::Value result;
  result["acked"] =
      Json::Int64(co_await service_.ackDeliveries(ctx.sub, body.notificationIds));
  co_return ApiResponse::ok(result);
}

drogon::Task<drogon::HttpResponsePtr>
NotificationController::deliverySummary(drogon::HttpRequestPtr req)
{
  const auto query = DeliverySummaryDto::fromRequest(req);
  co_return ApiResponse::ok(co_await service_.deliverySummary(query.since));
}
