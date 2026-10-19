#include "maintenance-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <feature/guard/dtos/declare-maintenance-dto.hxx>
#include <feature/guard/dtos/maintenance-view-dto.hxx>
#include <feature/guard/dtos/tamper-ask-answer-dto.hxx>
#include <http/api-response.hxx>
#include <utility>

namespace
{
MaintenanceActor actorOf(const drogon::HttpRequestPtr& req)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  return {.userId = jwt.sub, .role = jwt.role};
}
}

MaintenanceController::MaintenanceController(MaintenanceDependencies dependencies)
    : service_(std::move(dependencies))
{
}

drogon::Task<drogon::HttpResponsePtr> MaintenanceController::list(drogon::HttpRequestPtr req)
{
  co_return ApiResponse::ok(co_await service_.list(actorOf(req)));
}

drogon::Task<drogon::HttpResponsePtr> MaintenanceController::declare(drogon::HttpRequestPtr req)
{
  const auto body = DeclareMaintenanceDto::fromJson(*req->getJsonObject());
  co_return ApiResponse::created(co_await service_.declare({.actor = actorOf(req),
                                                            .cameraIds = body.cameraIds,
                                                            .environmentId = body.environmentId,
                                                            .minutes = body.minutes,
                                                            .source = MaintenanceSource::App}));
}

drogon::Task<drogon::HttpResponsePtr> MaintenanceController::end(drogon::HttpRequestPtr req, int64_t id)
{
  co_return ApiResponse::ok(co_await service_.end({.actor = actorOf(req), .id = id}));
}

drogon::Task<drogon::HttpResponsePtr> MaintenanceController::view(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = MaintenanceViewDto::fromJson(*req->getJsonObject());
  co_return ApiResponse::ok(co_await service_.answerView(
      {.actor = actorOf(req), .id = id, .cameraId = body.cameraId, .accept = body.accept}));
}

drogon::Task<drogon::HttpResponsePtr> MaintenanceController::claim(drogon::HttpRequestPtr req, int64_t id)
{
  co_return ApiResponse::created(co_await service_.claimIncident({.actor = actorOf(req), .incidentId = id}));
}

drogon::Task<drogon::HttpResponsePtr> MaintenanceController::answer(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = TamperAskAnswerDto::fromJson(*req->getJsonObject());
  co_return ApiResponse::ok(co_await service_.answerAsk(
      {.actor = actorOf(req),
       .askId = id,
       .answer = tamperAskAnswerFromString(body.answer).value_or(TamperAskAnswer::No)}));
}
