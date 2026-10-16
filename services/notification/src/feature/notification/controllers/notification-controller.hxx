#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/notification/services/notification-feature-service.hxx>
#include <memory>
#include <string>

class NotificationController
    : public drogon::HttpController<NotificationController, false>
{
public:
  explicit NotificationController(std::shared_ptr<const NotificationService> notifications);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(NotificationController::markAsRead, "/notification/read",
                drogon::Patch, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(NotificationController::ack, "/notification/ack",
                drogon::Patch, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(NotificationController::deliverySummary,
                "/notification/delivery-summary", drogon::Get, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(NotificationController::remove, "/notification/{1}",
                drogon::Delete, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(NotificationController::removeThread,
                "/notification/thread/{1}", drogon::Delete, "DeviceFilter",
                "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> markAsRead(drogon::HttpRequestPtr req);

  drogon::Task<drogon::HttpResponsePtr> ack(drogon::HttpRequestPtr req);

  drogon::Task<drogon::HttpResponsePtr> deliverySummary(
      drogon::HttpRequestPtr req);

  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req,
                                               int64_t id);

  drogon::Task<drogon::HttpResponsePtr> removeThread(drogon::HttpRequestPtr req,
                                                     std::string threadKey);

private:
  NotificationFeatureService service_;
};
