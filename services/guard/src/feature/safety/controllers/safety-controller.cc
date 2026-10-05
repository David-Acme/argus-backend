#include "safety-controller.hxx"

#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/linked-filter.hxx>
#include <auth/request-context.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <feature/safety/dtos/panic-dto.hxx>
#include <feature/safety/dtos/response-safety-dto.hxx>
#include <feature/safety/dtos/set-pin-dto.hxx>
#include <feature/safety/dtos/update-safety-dto.hxx>
#include <http/api-response.hxx>

#include <utility>

namespace
{
const JwtContext& callerOf(const drogon::HttpRequestPtr& req)
{
  return req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
}

Json::Value statusJson(const SafetyStatus& status)
{
  return ResponseSafetyStatusDto{.duressEnabled = status.duressEnabled, .hasPin = status.hasPin}
      .toJson();
}
}

SafetyController::SafetyController(std::shared_ptr<const SafetyService> service)
    : service_(std::move(service))
{
  auth_filters::requireLinked<DeviceFilter>();
  auth_filters::requireLinked<ValidJsonFilter>();
  auth_filters::requireLinked<JwtFilter>();
  auth_filters::requireLinked<RoleFilter>();
}

drogon::Task<drogon::HttpResponsePtr> SafetyController::panic(drogon::HttpRequestPtr req)
{
  const auto body = PanicDto::fromJson(*req->getJsonObject());
  const auto& caller = callerOf(req);
  const PanicResult result = co_await service_->panic(
      {.userId = caller.sub, .userName = caller.name, .environmentId = body.environmentId});
  co_return ApiResponse::ok(
      ResponsePanicDto{.alertId = result.alertId, .sent = result.sent, .repeated = result.repeated}
          .toJson());
}

drogon::Task<drogon::HttpResponsePtr> SafetyController::status(drogon::HttpRequestPtr req)
{
  co_return ApiResponse::ok(statusJson(co_await service_->status(callerOf(req).sub)));
}

drogon::Task<drogon::HttpResponsePtr> SafetyController::toggle(drogon::HttpRequestPtr req)
{
  const auto body = UpdateSafetyDto::fromJson(*req->getJsonObject());
  const auto& caller = callerOf(req);
  co_await service_->toggle({.duressEnabled = body.duressEnabled, .actorUserId = caller.sub});
  co_return ApiResponse::ok(statusJson(co_await service_->status(caller.sub)));
}

drogon::Task<drogon::HttpResponsePtr> SafetyController::setPin(drogon::HttpRequestPtr req)
{
  const auto body = SetPinDto::fromJson(*req->getJsonObject());
  co_return ApiResponse::ok(statusJson(co_await service_->setPin(
      {.userId = callerOf(req).sub, .disarmPin = body.disarmPin, .duressPin = body.duressPin})));
}

drogon::Task<drogon::HttpResponsePtr> SafetyController::removePin(drogon::HttpRequestPtr req)
{
  co_return ApiResponse::ok(statusJson(co_await service_->removePin(callerOf(req).sub)));
}
