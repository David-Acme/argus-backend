#include "calendar-event-share-controller.hxx"

#include <errors/response-exception.hxx>
#include <feature/api/calendar-event-share/dtos/create-calendar-event-share-dto.hxx>
#include <feature/api/calendar-event-share/dtos/update-calendar-event-share-dto.hxx>
#include <auth/jwt-filter.hxx>
#include <http/api-response.hxx>
#include <productivity/membership-error.hxx>
#include <productivity/productivity-errors.hxx>
#include <auth/request-context.hxx>

namespace
{
drogon::HttpResponsePtr failureFor(MembershipError error)
{
  switch (error) {
    case MembershipError::UserNotFound:
      throw ResponseException(ProductivityErrors::UserNotFound);
    case MembershipError::UserNotAllowed:
      throw ResponseException(ProductivityErrors::CannotSeeCalendarEvents);
    case MembershipError::SelfShare:
      throw ResponseException(ProductivityErrors::OwnerAlreadyHasAccess);
    default:
      throw ResponseException(ProductivityErrors::CalendarEventNotFound);
  }
}
} // namespace

drogon::Task<drogon::HttpResponsePtr>
CalendarEventShareController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateCalendarEventShareDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto result = co_await service_.create(body, ctx.sub);
  if (!result.row)
    co_return failureFor(result.error);
  co_return ApiResponse::ok(result.row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CalendarEventShareController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateCalendarEventShareDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  const auto result = co_await service_.update(
      {.id = id, .body = body, .actorId = ctx.sub});
  if (!result.row)
    co_return failureFor(result.error);
  co_return ApiResponse::ok(result.row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
CalendarEventShareController::remove(drogon::HttpRequestPtr req, int64_t id)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  if (!co_await service_.remove(id, ctx.sub))
    throw ResponseException(ProductivityErrors::ShareNotFound);

  Json::Value result;
  result["deleted"] = true;
  co_return ApiResponse::ok(result);
}
