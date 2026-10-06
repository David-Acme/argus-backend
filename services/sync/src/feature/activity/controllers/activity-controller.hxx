#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/activity/services/activity-feature-service.hxx>

class ActivityController : public drogon::HttpController<ActivityController, false>
{
public:
  ActivityController();

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ActivityController::list, "/sync/activity", drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);

private:
  ActivityFeatureService service_;
};
