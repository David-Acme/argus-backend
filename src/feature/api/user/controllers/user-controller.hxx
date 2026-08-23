#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/user/services/user-feature-service.hxx>

class UserController : public drogon::HttpController<UserController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(UserController::list, "/user", drogon::Get, "DeviceFilter",
                "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(UserController::update, "/user/{1}", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(UserController::deactivate, "/user/{1}", drogon::Delete,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  update(drogon::HttpRequestPtr req, int64_t userId);
  drogon::Task<drogon::HttpResponsePtr>
  deactivate(drogon::HttpRequestPtr req, int64_t userId);

private:
  UserFeatureService service_;
};
