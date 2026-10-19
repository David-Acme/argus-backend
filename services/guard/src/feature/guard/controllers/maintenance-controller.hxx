#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/guard/services/maintenance-service.hxx>

class MaintenanceController
    : public drogon::HttpController<MaintenanceController, false>
{
public:
  explicit MaintenanceController(MaintenanceDependencies dependencies);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(MaintenanceController::list, "/guard/maintenance", drogon::Get,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(MaintenanceController::declare, "/guard/maintenance", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(MaintenanceController::end, "/guard/maintenance/{1}", drogon::Delete,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(MaintenanceController::view, "/guard/maintenance/{1}/view", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(MaintenanceController::claim, "/guard/incidents/{1}/mine", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(MaintenanceController::answer, "/guard/tamper/asks/{1}", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> declare(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> end(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> view(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> claim(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> answer(drogon::HttpRequestPtr req, int64_t id);

private:
  MaintenanceService service_;
};
