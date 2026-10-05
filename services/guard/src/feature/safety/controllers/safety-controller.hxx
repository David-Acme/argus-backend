#pragma once

#include <feature/safety/services/safety-service.hxx>

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <memory>

class SafetyController : public drogon::HttpController<SafetyController, false>
{
public:
  explicit SafetyController(std::shared_ptr<const SafetyService> service);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(SafetyController::panic, "/guard/panic", drogon::Post, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(SafetyController::status, "/guard/safety", drogon::Get, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(SafetyController::toggle, "/guard/safety", drogon::Patch, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(SafetyController::setPin, "/guard/safety/pin", drogon::Put, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(SafetyController::removePin, "/guard/safety/pin", drogon::Delete,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> panic(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> status(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> toggle(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> setPin(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> removePin(drogon::HttpRequestPtr req);

private:
  std::shared_ptr<const SafetyService> service_;
};
