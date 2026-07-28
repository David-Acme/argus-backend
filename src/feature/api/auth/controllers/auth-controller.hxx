#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/auth/services/auth-service.hxx>

class AuthController : public drogon::HttpController<AuthController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(AuthController::login, "/auth/login", drogon::Post,
                "DeviceFilter");
  ADD_METHOD_TO(AuthController::status, "/auth/status", drogon::Get,
                "DeviceFilter", "JwtFilter");
  ADD_METHOD_TO(AuthController::refreshToken, "/auth/refresh-token",
                drogon::Patch, "DeviceFilter", "ValidJsonFilter");
  ADD_METHOD_TO(AuthController::logout, "/auth/logout", drogon::Patch,
                "DeviceFilter", "JwtFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> login(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> status(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  refreshToken(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> logout(drogon::HttpRequestPtr req);

private:
  AuthService service_;
};
