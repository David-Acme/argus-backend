#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/call/services/call-engine.hxx>

#include <memory>

class CallResponseController
    : public drogon::HttpController<CallResponseController, false>
{
public:
  explicit CallResponseController(std::shared_ptr<CallEngine> engine);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(CallResponseController::list, "/notification/responses",
                drogon::Get, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(CallResponseController::read, "/notification/responses/{1}",
                drogon::Get, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(CallResponseController::decide, "/notification/responses/{1}",
                drogon::Patch, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);

  drogon::Task<drogon::HttpResponsePtr> read(drogon::HttpRequestPtr req,
                                             int64_t responseId);

  drogon::Task<drogon::HttpResponsePtr> decide(drogon::HttpRequestPtr req,
                                               int64_t responseId);

private:
  std::shared_ptr<CallEngine> service_;
};
