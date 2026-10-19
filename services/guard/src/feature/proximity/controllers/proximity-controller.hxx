#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

#include <cstdint>

class ProximityService;

class ProximityController : public drogon::HttpController<ProximityController, false>
{
public:
  explicit ProximityController(ProximityService* service);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ProximityController::me, "/guard/proximity/me", drogon::Get, "DeviceFilter", "ValidJsonFilter",
                "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProximityController::setMe, "/guard/proximity/me", drogon::Put, "DeviceFilter", "ValidJsonFilter",
                "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProximityController::people, "/guard/proximity/people", drogon::Get, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProximityController::receivers, "/guard/proximity/receivers", drogon::Get, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProximityController::addReceiver, "/guard/proximity/receivers", drogon::Post, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProximityController::removeReceiver, "/guard/proximity/receivers/{1}", drogon::Delete,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProximityController::sensors, "/guard/proximity/sensors", drogon::Get, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProximityController::addSensor, "/guard/proximity/sensors", drogon::Post, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProximityController::removeSensor, "/guard/proximity/sensors/{1}", drogon::Delete, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> me(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> setMe(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> people(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> receivers(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> addReceiver(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> removeReceiver(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> sensors(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> addSensor(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> removeSensor(drogon::HttpRequestPtr req, int64_t id);

private:
  ProximityService* service_{nullptr};
};
