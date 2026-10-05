#include "presence-controller.hxx"

#include <feature/presence/dtos/response-presence-dto.hxx>
#include <feature/presence/services/presence-service.hxx>
#include <http/api-response.hxx>

PresenceController::PresenceController(const PresenceService* service)
    : service_(service)
{
}

drogon::Task<drogon::HttpResponsePtr>
PresenceController::list(drogon::HttpRequestPtr)
{
  const ResponsePresenceDto result{.people = co_await service_->snapshot({})};
  co_return ApiResponse::ok(result.toJson());
}
