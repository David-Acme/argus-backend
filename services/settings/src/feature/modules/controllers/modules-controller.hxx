#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/modules/services/module-engine.hxx>

#include <string>
#include <string_view>

class ModulesController : public drogon::HttpController<ModulesController, false>
{
public:
  explicit ModulesController(ModuleEngine* engine);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ModulesController::list, "/modules", drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ModulesController::install, "/modules/{1}/install", drogon::Post, "DeviceFilter", "ValidJsonFilter",
                "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ModulesController::pause, "/modules/{1}/pause", drogon::Post, "DeviceFilter", "ValidJsonFilter",
                "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ModulesController::resume, "/modules/{1}/resume", drogon::Post, "DeviceFilter", "ValidJsonFilter",
                "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ModulesController::cancel, "/modules/{1}/cancel", drogon::Post, "DeviceFilter", "ValidJsonFilter",
                "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ModulesController::disable, "/modules/{1}/disable", drogon::Post, "DeviceFilter", "ValidJsonFilter",
                "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ModulesController::uninstall, "/modules/{1}/uninstall", drogon::Post, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ModulesController::data, "/modules/{1}/data", drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ModulesController::impact, "/modules/{1}/impact", drogon::Get, "DeviceFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(ModulesController::requestModule, "/modules/{1}/request", drogon::Post, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> install(drogon::HttpRequestPtr req, std::string id);
  drogon::Task<drogon::HttpResponsePtr> pause(drogon::HttpRequestPtr req, std::string id);
  drogon::Task<drogon::HttpResponsePtr> resume(drogon::HttpRequestPtr req, std::string id);
  drogon::Task<drogon::HttpResponsePtr> cancel(drogon::HttpRequestPtr req, std::string id);
  drogon::Task<drogon::HttpResponsePtr> disable(drogon::HttpRequestPtr req, std::string id);
  drogon::Task<drogon::HttpResponsePtr> uninstall(drogon::HttpRequestPtr req, std::string id);
  drogon::Task<drogon::HttpResponsePtr> data(drogon::HttpRequestPtr req, std::string id);
  drogon::Task<drogon::HttpResponsePtr> impact(drogon::HttpRequestPtr req, std::string id);
  drogon::Task<drogon::HttpResponsePtr> requestModule(drogon::HttpRequestPtr req, std::string id);

  [[nodiscard]] static std::string_view languageOf(const drogon::HttpRequestPtr& req);

private:
  [[nodiscard]] ModuleEngine& engine() const;

  ModuleEngine* engine_;
};
