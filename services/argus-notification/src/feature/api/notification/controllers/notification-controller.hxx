#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/notification/services/notification-feature-service.hxx>

class NotificationController
    : public drogon::HttpController<NotificationController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(NotificationController::markAsRead, "/notification/read",
                drogon::Patch, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> markAsRead(drogon::HttpRequestPtr req);

private:
  NotificationFeatureService service_;
};
