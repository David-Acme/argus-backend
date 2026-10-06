#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/settings/services/settings-gateway-service.hxx>
#include <feature/settings/services/settings-profile-service.hxx>

#include <functional>
#include <optional>
#include <string>

struct SettingsControllerInput
{
  SettingsGatewayInput gateway;
  std::optional<ProfileCatalog> profiles;
  HardwareFacts hardware;
  std::function<bool(const std::string&)> ownerVisible{};
};

class SettingsController : public drogon::HttpController<SettingsController, false>
{
public:
  explicit SettingsController(const SettingsControllerInput& input);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(SettingsController::list, "/settings", drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(SettingsController::profiles, "/settings/profiles", drogon::Get, "DeviceFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(SettingsController::revertRecommended, "/settings/profiles/recommended/revert", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(SettingsController::applyProfile, "/settings/profiles/{1}/apply", drogon::Post, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(SettingsController::update, "/settings/{1}", drogon::Patch, "DeviceFilter", "ValidJsonFilter",
                "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> profiles(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> revertRecommended(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> applyProfile(drogon::HttpRequestPtr req, std::string id);
  drogon::Task<drogon::HttpResponsePtr> update(drogon::HttpRequestPtr req, std::string owner);

private:
  SettingsGatewayService service_;
  SettingsProfileService profiles_;
  std::function<bool(const std::string&)> ownerVisible_;
};
