#include "visitor-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <feature/visitor/dtos/list-visitors-dto.hxx>
#include <http/api-response.hxx>

#include <algorithm>

namespace
{
VisitorRequester requesterOf(const drogon::HttpRequestPtr& req)
{
  const auto& jwt = req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
  return {.userId = jwt.sub, .role = jwt.role};
}

int64_t sampleIdOf(const drogon::HttpRequestPtr& req)
{
  const std::string value = req->getParameter("sampleId");
  if (value.empty() || value.size() > 18 ||
      !std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; }))
    return 0;
  return std::stoll(value);
}
}

drogon::Task<drogon::HttpResponsePtr> VisitorController::list(drogon::HttpRequestPtr req)
{
  const auto query = ListVisitorsDto::fromRequest(req);
  const auto result = co_await service_.list(
      {.requester = requesterOf(req),
       .namedOnly = query.scope == "named",
       .page = {.filter = visitorListFilterFromString(query.filter)
                              .value_or(VisitorListFilter::All),
                .search = query.search,
                .after = {.lastSeenAt = query.beforeSeen, .id = query.beforeId},
                .limit = query.limit}});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VisitorController::settings(drogon::HttpRequestPtr)
{
  co_return ApiResponse::ok((co_await service_.settings()).toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VisitorController::updateSettings(drogon::HttpRequestPtr req)
{
  const auto body = UpdateVisitorSettingsDto::fromJson(*req->getJsonObject());
  const auto result = co_await service_.updateSettings(
      {.requester = requesterOf(req), .body = body});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VisitorController::detail(drogon::HttpRequestPtr req, int64_t personId)
{
  const auto result = co_await service_.detail(
      {.requester = requesterOf(req), .personId = personId});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VisitorController::update(drogon::HttpRequestPtr req, int64_t personId)
{
  const auto body = UpdateVisitorDto::fromJson(*req->getJsonObject());
  const auto result = co_await service_.update(
      {.requester = requesterOf(req), .personId = personId, .body = body});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VisitorController::remove(drogon::HttpRequestPtr req, int64_t personId)
{
  co_await service_.remove({.requester = requesterOf(req), .personId = personId});
  co_return ApiResponse::noContent();
}

drogon::Task<drogon::HttpResponsePtr>
VisitorController::merge(drogon::HttpRequestPtr req, int64_t personId)
{
  const auto body = MergeVisitorsDto::fromJson(*req->getJsonObject());
  const auto result = co_await service_.merge(
      {.requester = requesterOf(req), .personId = personId, .body = body});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VisitorController::split(drogon::HttpRequestPtr req, int64_t personId)
{
  const auto body = SplitVisitorDto::fromJson(*req->getJsonObject());
  const auto result = co_await service_.split(
      {.requester = requesterOf(req), .personId = personId, .body = body});
  co_return ApiResponse::created(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VisitorController::removeSample(drogon::HttpRequestPtr req, int64_t personId,
                                int64_t sampleId)
{
  co_await service_.removeSample(
      {.requester = requesterOf(req), .personId = personId, .sampleId = sampleId});
  co_return ApiResponse::noContent();
}

drogon::Task<drogon::HttpResponsePtr>
VisitorController::mintCrop(drogon::HttpRequestPtr req, int64_t personId)
{
  const auto result = co_await service_.mintCrop({.requester = requesterOf(req),
                                                  .personId = personId,
                                                  .sampleId = sampleIdOf(req)});
  co_return ApiResponse::ok(result.toJson());
}

drogon::Task<drogon::HttpResponsePtr>
VisitorController::consumeCrop(drogon::HttpRequestPtr req, std::string token)
{
  const auto result = co_await service_.consumeCrop(
      {.requester = requesterOf(req), .token = std::move(token)});
  co_return ApiResponse::ok(result.toJson());
}
