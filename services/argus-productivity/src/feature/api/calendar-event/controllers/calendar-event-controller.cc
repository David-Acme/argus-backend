#include "calendar-event-controller.hxx"

#include <config/app-config.hxx>
#include <feature/api/calendar-event/dtos/create-calendar-event-dto.hxx>
#include <feature/api/calendar-event/dtos/update-calendar-event-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

drogon::Task<drogon::HttpResponsePtr>
CalendarEventController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateCalendarEventDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  const auto row =
      co_await service_.create(body, {.ownerId = ctx.sub, .actorId = ctx.sub});
  co_return ApiResponse::ok(row.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CalendarEventController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateCalendarEventDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  const auto row = co_await service_.update(id, body, ctx.sub);
  if (!row)
    co_return AppConfig::get404Response("Calendar event not found");
  co_return ApiResponse::ok(row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CalendarEventController::remove(drogon::HttpRequestPtr req, int64_t id)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  if (!co_await service_.remove(id, ctx.sub))
    co_return AppConfig::get404Response("Calendar event not found");

  Json::Value result;
  result["deleted"] = true;
  co_return ApiResponse::ok(result);
}
