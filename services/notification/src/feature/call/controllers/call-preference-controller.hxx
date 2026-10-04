#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/call/services/call-preference-service.hxx>

class CallPreferenceController
    : public drogon::HttpController<CallPreferenceController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(CallPreferenceController::read,
                "/notification/call-preferences", drogon::Get, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CallPreferenceController::update,
                "/notification/call-preferences", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> read(drogon::HttpRequestPtr req);

  drogon::Task<drogon::HttpResponsePtr> update(drogon::HttpRequestPtr req);

private:
  CallPreferenceService service_;
};
