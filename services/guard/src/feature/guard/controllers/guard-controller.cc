#include "guard-controller.hxx"

#include "guard-errors.hxx"

#include <errors/response-exception.hxx>
#include <auth/device-filter.hxx>
#include <http/api-response.hxx>
#include <identity/identity-client.hxx>
#include <feature/guard/dtos/create-expected-guest-dto.hxx>
#include <feature/guard/dtos/feedback-decision-dto.hxx>
#include <feature/guard/dtos/list-decisions-dto.hxx>
#include <feature/guard/dtos/list-episodes-dto.hxx>
#include <feature/guard/dtos/list-incidents-dto.hxx>
#include <feature/guard/dtos/review-episode-dto.hxx>
#include <feature/guard/dtos/update-camera-context-dto.hxx>
#include <feature/guard/dtos/update-guard-site-dto.hxx>
#include <feature/guard/dtos/summary-decisions-dto.hxx>
#include <feature/guard/dtos/remove-expected-guest-dto.hxx>
#include <feature/guard/dtos/update-guard-mode-dto.hxx>
#include <auth/request-context.hxx>

GuardController::GuardController(const GuardFeatureDependencies& dependencies)
    : service_(dependencies)
{
}

namespace
{
std::string bearerToken(const drogon::HttpRequestPtr& request)
{
  const std::string header = request->getHeader("authorization");
  constexpr std::string_view kPrefix = "Bearer ";
  if (header.rfind(kPrefix, 0) != 0)
    return {};
  return header.substr(kPrefix.size());
}
}

drogon::Task<drogon::HttpResponsePtr> GuardController::promotePerson(
    drogon::HttpRequestPtr req, int64_t personId)
{
  const std::string token = bearerToken(req);
  if (token.empty())
    throw ResponseException(GuardErrors::OwnerAccessTokenRequired);
  const auto& device =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
  const bool promoted = co_await service_.promotePerson(
      {.personId = personId, .accessToken = token, .deviceHash = device.deviceHash});
  Json::Value response;
  response["promoted"] = promoted;
  co_return ApiResponse::ok(response);
}

drogon::Task<drogon::HttpResponsePtr> GuardController::mode(
    drogon::HttpRequestPtr)
{
  co_return ApiResponse::ok(co_await service_.mode());
}

drogon::Task<drogon::HttpResponsePtr> GuardController::setMode(
    drogon::HttpRequestPtr req)
{
  const auto body = UpdateGuardModeDto::fromJson(*req->getJsonObject());
  Json::Value response;
  response["mode"] = co_await service_.setMode(body.mode);
  co_return ApiResponse::ok(response);
}

drogon::Task<drogon::HttpResponsePtr> GuardController::incidents(
    drogon::HttpRequestPtr req)
{
  const auto query = ListIncidentsDto::fromRequest(req);
  co_return ApiResponse::ok(co_await service_.incidents(query.limit));
}

drogon::Task<drogon::HttpResponsePtr> GuardController::decisions(
    drogon::HttpRequestPtr req)
{
  const auto query = ListDecisionsDto::fromRequest(req);
  co_return ApiResponse::ok(co_await service_.decisions(query));
}

drogon::Task<drogon::HttpResponsePtr> GuardController::decisionsSummary(
    drogon::HttpRequestPtr req)
{
  const auto query = SummaryDecisionsDto::fromRequest(req);
  co_return ApiResponse::ok(co_await service_.decisionsSummary(
      {.from = query.from,
       .to = query.to,
       .nearMissMargin = query.nearMissMargin}));
}

drogon::Task<drogon::HttpResponsePtr> GuardController::feedback(
    drogon::HttpRequestPtr req, const std::string& eventId)
{
  const auto body = FeedbackDecisionDto::fromJson(*req->getJsonObject());
  if (!co_await service_.setFeedback(eventId, body.label))
    throw ResponseException(GuardErrors::DecisionNotFound);
  co_return ApiResponse::ok(Json::Value(Json::objectValue));
}

drogon::Task<drogon::HttpResponsePtr> GuardController::createGuest(
    drogon::HttpRequestPtr req)
{
  const auto body = CreateExpectedGuestDto::fromJson(*req->getJsonObject());
  Json::Value response;
  response["id"] = Json::Int64(co_await service_.createGuest(body));
  co_return ApiResponse::ok(response);
}

drogon::Task<drogon::HttpResponsePtr> GuardController::listGuests(
    drogon::HttpRequestPtr)
{
  co_return ApiResponse::ok(co_await service_.guests());
}

drogon::Task<drogon::HttpResponsePtr> GuardController::removeGuest(
    drogon::HttpRequestPtr req)
{
  const auto query = RemoveExpectedGuestDto::fromRequest(req);
  const bool removed = co_await service_.removeGuest(query.id);
  if (!removed)
    throw ResponseException(GuardErrors::ExpectedGuestNotFound);
  Json::Value response;
  response["removed"] = true;
  co_return ApiResponse::ok(response);
}

drogon::Task<drogon::HttpResponsePtr> GuardController::site(
    drogon::HttpRequestPtr)
{
  co_return ApiResponse::ok(co_await service_.site());
}

drogon::Task<drogon::HttpResponsePtr> GuardController::updateSite(
    drogon::HttpRequestPtr req)
{
  const auto body = UpdateGuardSiteDto::fromJson(*req->getJsonObject());
  co_return ApiResponse::ok(co_await service_.updateSite(body));
}

drogon::Task<drogon::HttpResponsePtr> GuardController::cameras(
    drogon::HttpRequestPtr)
{
  co_return ApiResponse::ok(co_await service_.cameras());
}

drogon::Task<drogon::HttpResponsePtr> GuardController::setCamera(
    drogon::HttpRequestPtr req, int64_t cameraId)
{
  if (cameraId <= 0)
    throw ResponseException(GuardErrors::CameraIdInvalid);
  const auto body = UpdateCameraContextDto::fromJson(*req->getJsonObject());
  co_return ApiResponse::ok(
      co_await service_.setCamera({.cameraId = cameraId, .context = body}));
}

drogon::Task<drogon::HttpResponsePtr> GuardController::episodes(
    drogon::HttpRequestPtr req)
{
  const auto query = ListEpisodesDto::fromRequest(req);
  co_return ApiResponse::ok(co_await service_.episodes(query));
}

drogon::Task<drogon::HttpResponsePtr> GuardController::episode(
    drogon::HttpRequestPtr, int64_t episodeId)
{
  const auto found = co_await service_.episode(episodeId);
  if (!found)
    throw ResponseException(GuardErrors::EpisodeNotFound);
  co_return ApiResponse::ok(*found);
}

drogon::Task<drogon::HttpResponsePtr> GuardController::reviewEpisode(
    drogon::HttpRequestPtr req, int64_t episodeId)
{
  const auto body = ReviewEpisodeDto::fromJson(*req->getJsonObject());
  const auto reviewed = co_await service_.reviewEpisode(
      {.episodeId = episodeId,
       .label = feedbackLabelFromString(body.label)
                    .value_or(FeedbackLabel::Useful)});
  if (!reviewed)
    throw ResponseException(GuardErrors::EpisodeNotFound);
  co_return ApiResponse::ok(*reviewed);
}
