#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/calendar-event-share/services/calendar-event-share-feature-service.hxx>

class CalendarEventShareController : public drogon::HttpController<CalendarEventShareController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(CalendarEventShareController::create, "/calendar-event-share", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CalendarEventShareController::update, "/calendar-event-share/{1}", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CalendarEventShareController::remove, "/calendar-event-share/{1}", drogon::Delete,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> create(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> update(drogon::HttpRequestPtr req,
                                               int64_t id);
  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req,
                                               int64_t id);

private:
  CalendarEventShareFeatureService service_;
};
