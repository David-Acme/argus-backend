#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/notification/services/notification-token-feature-service.hxx>

class NotificationTokenController
    : public drogon::HttpController<NotificationTokenController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(NotificationTokenController::registerToken,
                "/notification-token", drogon::Post, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr>
  registerToken(drogon::HttpRequestPtr req);

private:
  NotificationTokenFeatureService service_;
};
