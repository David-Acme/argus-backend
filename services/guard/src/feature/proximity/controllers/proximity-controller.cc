#include "proximity-controller.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <feature/proximity/dtos/create-proximity-receiver-dto.hxx>
#include <feature/proximity/dtos/create-proximity-sensor-dto.hxx>
#include <feature/proximity/dtos/response-proximity-dto.hxx>
#include <feature/proximity/dtos/update-proximity-me-dto.hxx>
#include <feature/proximity/services/proximity-service.hxx>
#include <http/api-response.hxx>

#include <utility>

namespace
{
const JwtContext& callerOf(const drogon::HttpRequestPtr& req)
{
  return req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);
}
}

ProximityController::ProximityController(ProximityService* service) : service_(service) {}

drogon::Task<drogon::HttpResponsePtr> ProximityController::me(drogon::HttpRequestPtr req)
{
  const ProximityMeView view = co_await service_->me(callerOf(req).sub);
  co_return ApiResponse::ok(ResponseProximityMeDto{.me = view}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ProximityController::setMe(drogon::HttpRequestPtr req)
{
  const auto body = UpdateProximityMeDto::fromJson(*req->getJsonObject());
  const ProximityMeView view = co_await service_->setBle({.userId = callerOf(req).sub, .enabled = body.ble});
  co_return ApiResponse::ok(ResponseProximityMeDto{.me = view}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ProximityController::people(drogon::HttpRequestPtr)
{
  co_return ApiResponse::ok(ResponseProximityPeopleDto{.people = co_await service_->people()}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ProximityController::receivers(drogon::HttpRequestPtr)
{
  co_return ApiResponse::ok(ResponseProximityReceiversDto{.receivers = co_await service_->receivers()}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ProximityController::addReceiver(drogon::HttpRequestPtr req)
{
  auto body = CreateProximityReceiverDto::fromJson(*req->getJsonObject());
  const ProximityReceiverView view =
      co_await service_->addReceiver({.name = std::move(body.name),
                                      .room = std::move(body.room),
                                      .kind = proximityReceiverKindFromString(body.kind).value_or(
                                          ProximityReceiverKind::Esphome),
                                      .host = std::move(body.host),
                                      .port = body.port});
  co_return ApiResponse::created(ResponseProximityReceiverDto{.receiver = view}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ProximityController::removeReceiver(drogon::HttpRequestPtr, int64_t id)
{
  co_await service_->removeReceiver(id);
  co_return ApiResponse::noContent();
}

drogon::Task<drogon::HttpResponsePtr> ProximityController::sensors(drogon::HttpRequestPtr)
{
  co_return ApiResponse::ok(ResponseProximitySensorsDto{.sensors = co_await service_->sensors()}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ProximityController::addSensor(drogon::HttpRequestPtr req)
{
  auto body = CreateProximitySensorDto::fromJson(*req->getJsonObject());
  const ProximitySensorView view = co_await service_->addSensor(
      {.name = std::move(body.name),
       .room = std::move(body.room),
       .model = proximitySensorModelFromString(body.model).value_or(ProximitySensorModel::Ld2410),
       .source = proximitySensorSourceFromString(body.source).value_or(ProximitySensorSource::Esphome),
       .host = std::move(body.host),
       .port = body.port,
       .entityId = std::move(body.entityId),
       .cameraId = body.cameraId,
       .panOffsetDeg = body.panOffsetDeg});
  co_return ApiResponse::created(ResponseProximitySensorDto{.sensor = view}.toJson());
}

drogon::Task<drogon::HttpResponsePtr> ProximityController::removeSensor(drogon::HttpRequestPtr, int64_t id)
{
  co_await service_->removeSensor(id);
  co_return ApiResponse::noContent();
}
