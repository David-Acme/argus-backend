#include "guard-controller.hxx"

#include "guard-errors.hxx"

#include <errors/response-exception.hxx>
#include <auth/device-filter.hxx>
#include <http/api-response.hxx>
#include <identity/identity-client.hxx>
#include <feature/api/guard/dtos/create-expected-guest-dto.hxx>
#include <feature/api/guard/dtos/feedback-decision-dto.hxx>
#include <feature/api/guard/dtos/list-decisions-dto.hxx>
#include <feature/api/guard/dtos/list-incidents-dto.hxx>
#include <feature/api/guard/dtos/summary-decisions-dto.hxx>
#include <feature/api/guard/dtos/remove-expected-guest-dto.hxx>
#include <feature/api/guard/dtos/update-guard-mode-dto.hxx>
#include <auth/request-context.hxx>

GuardController::GuardController(IdentityClient* identity)
    : service_(identity)
{
}

namespace
{
// Owner bearer token and device fingerprint forwarded to argus-identity.
std::string bearerToken(const drogon::HttpRequestPtr& request)
{
  const std::string header = request->getHeader("authorization");
  constexpr std::string_view kPrefix = "Bearer ";
  if (header.rfind(kPrefix, 0) != 0)
    return {};
  return header.substr(kPrefix.size());
}
} // namespace

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
  Json::Value response;
  response["mode"] = co_await service_.mode();
  co_return ApiResponse::ok(response);
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
