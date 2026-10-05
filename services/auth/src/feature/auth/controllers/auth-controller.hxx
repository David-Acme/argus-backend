#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/auth/services/auth-feature-service.hxx>
#include <feature/auth/services/session-management-service.hxx>
#include <cstdint>
#include <string>

class AuthController : public drogon::HttpController<AuthController, false>
{
public:
  AuthController(const IdentityClient* identity,
                 AuthFeatureService::Config config);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(AuthController::login, "/auth/login", drogon::Post,
                "DeviceFilter");
  ADD_METHOD_TO(AuthController::registerUser, "/auth/register", drogon::Post,
                "DeviceFilter");
  ADD_METHOD_TO(AuthController::status, "/auth/status", drogon::Get,
                "DeviceFilter", "JwtFilter");
  ADD_METHOD_TO(AuthController::createDeviceLogin, "/auth/device-login",
                drogon::Post, "DeviceFilter");
  ADD_METHOD_TO(AuthController::deviceLoginDetails,
                "/auth/device-login/{1}/details", drogon::Get, "DeviceFilter",
                "JwtFilter");
  ADD_METHOD_TO(AuthController::approveDeviceLogin,
                "/auth/device-login/{1}/approve", drogon::Post, "DeviceFilter",
                "JwtFilter");
  ADD_METHOD_TO(AuthController::pollDeviceLogin, "/auth/device-login/{1}",
                drogon::Get, "DeviceFilter");
  ADD_METHOD_TO(AuthController::refreshToken, "/auth/refresh-token",
                drogon::Patch, "DeviceFilter", "ValidJsonFilter");
  ADD_METHOD_TO(AuthController::logout, "/auth/logout", drogon::Patch,
                "DeviceFilter", "JwtFilter");
  ADD_METHOD_TO(AuthController::updateMe, "/auth/me", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter");
  ADD_METHOD_TO(AuthController::listSessions, "/auth/sessions", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(AuthController::revokeSessions, "/auth/sessions",
                drogon::Delete, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(AuthController::revokeSession, "/auth/sessions/{1}",
                drogon::Delete, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(AuthController::listEveryUserSessions, "/auth/users/sessions",
                drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(AuthController::listUserSessions, "/auth/users/{1}/sessions",
                drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(AuthController::revokeUserSessions, "/auth/users/{1}/sessions",
                drogon::Delete, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(AuthController::revokeUserSession,
                "/auth/users/{1}/sessions/{2}", drogon::Delete, "DeviceFilter",
                "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> login(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  registerUser(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> status(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  createDeviceLogin(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  deviceLoginDetails(drogon::HttpRequestPtr req, std::string challengeId);
  drogon::Task<drogon::HttpResponsePtr>
  approveDeviceLogin(drogon::HttpRequestPtr req, std::string challengeId);
  drogon::Task<drogon::HttpResponsePtr>
  pollDeviceLogin(drogon::HttpRequestPtr req, std::string challengeId);
  drogon::Task<drogon::HttpResponsePtr>
  refreshToken(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> logout(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> updateMe(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  listSessions(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  revokeSessions(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  revokeSession(drogon::HttpRequestPtr req, std::string sessionId);
  drogon::Task<drogon::HttpResponsePtr>
  listEveryUserSessions(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  listUserSessions(drogon::HttpRequestPtr req, int64_t userId);
  drogon::Task<drogon::HttpResponsePtr>
  revokeUserSessions(drogon::HttpRequestPtr req, int64_t userId);
  drogon::Task<drogon::HttpResponsePtr>
  revokeUserSession(drogon::HttpRequestPtr req, int64_t userId,
                    std::string sessionId);

private:
  AuthFeatureService authService_;
  SessionManagementService sessionService_;
};
