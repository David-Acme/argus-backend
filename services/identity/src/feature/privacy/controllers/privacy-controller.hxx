#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/privacy/services/privacy-feature-service.hxx>

class PrivacyController : public drogon::HttpController<PrivacyController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(PrivacyController::me, "/privacy/me", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(PrivacyController::decide, "/privacy/me", drogon::Put,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(PrivacyController::directory, "/privacy/users", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(PrivacyController::household, "/privacy/household",
                drogon::Patch, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> me(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> decide(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> directory(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> household(drogon::HttpRequestPtr req);

private:
  PrivacyFeatureService service_;
};
