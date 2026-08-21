#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/calendar-event/services/calendar-event-feature-service.hxx>

class CalendarEventController
    : public drogon::HttpController<CalendarEventController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(CalendarEventController::create, "/calendar-event",
                drogon::Post, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(CalendarEventController::update, "/calendar-event/{1}",
                drogon::Patch, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(CalendarEventController::remove, "/calendar-event/{1}",
                drogon::Delete, "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> create(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> update(drogon::HttpRequestPtr req,
                                               int64_t id);
  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req,
                                               int64_t id);

private:
  CalendarEventFeatureService service_;
};
