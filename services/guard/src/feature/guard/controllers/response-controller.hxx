#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/guard/services/response-feature-service.hxx>

class ResponseController
    : public drogon::HttpController<ResponseController, false>
{
public:
  explicit ResponseController(ResponseFeatureDependencies dependencies);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ResponseController::view, "/guard/environments/{1}/response",
                drogon::Get, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(ResponseController::replace, "/guard/environments/{1}/response",
                drogon::Put, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(ResponseController::duty, "/guard/environments/{1}/duty",
                drogon::Post, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> view(drogon::HttpRequestPtr req,
                                             int64_t environmentId);
  drogon::Task<drogon::HttpResponsePtr> replace(drogon::HttpRequestPtr req,
                                                int64_t environmentId);
  drogon::Task<drogon::HttpResponsePtr> duty(drogon::HttpRequestPtr req,
                                             int64_t environmentId);

private:
  ResponseFeatureService service_;
};
