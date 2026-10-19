#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/camera-control/services/camera-patrol-service.hxx>

class CameraPatrolController
    : public drogon::HttpController<CameraPatrolController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(CameraPatrolController::read, "/camera/{1}/patrol", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraPatrolController::change, "/camera/{1}/patrol", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraPatrolController::remove, "/camera/{1}/patrol", drogon::Delete,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> read(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> change(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req, int64_t id);

private:
  CameraPatrolService service_;
};
