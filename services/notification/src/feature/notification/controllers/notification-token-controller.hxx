#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/notification/services/notification-token-feature-service.hxx>

class NotificationTokenController
    : public drogon::HttpController<NotificationTokenController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(NotificationTokenController::registerToken,
                "/notification-token", drogon::Post, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(NotificationTokenController::unregisterToken,
                "/notification-token", drogon::Delete, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr>
  registerToken(drogon::HttpRequestPtr req);

  drogon::Task<drogon::HttpResponsePtr>
  unregisterToken(drogon::HttpRequestPtr req);

private:
  NotificationTokenFeatureService service_;
};
