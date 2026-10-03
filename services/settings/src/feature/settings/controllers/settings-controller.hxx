#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/settings/services/settings-gateway-service.hxx>

#include <string>

class SettingsController : public drogon::HttpController<SettingsController, false>
{
public:
  explicit SettingsController(const SettingsGatewayInput& input);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(SettingsController::list, "/settings", drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(SettingsController::update, "/settings/{1}", drogon::Patch, "DeviceFilter", "ValidJsonFilter",
                "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> update(drogon::HttpRequestPtr req, std::string owner);

private:
  SettingsGatewayService service_;
};
