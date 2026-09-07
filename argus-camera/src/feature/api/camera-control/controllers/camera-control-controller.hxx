#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/camera-control/services/camera-control-feature-service.hxx>

class CameraControlController
    : public drogon::HttpController<CameraControlController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(CameraControlController::status, "/camera/{1}/status", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraControlController::presets, "/camera/{1}/presets", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraControlController::move, "/camera/{1}/ptz", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraControlController::preset, "/camera/{1}/preset", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraControlController::settings, "/camera/{1}/settings", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraControlController::capabilities, "/camera/{1}/capabilities",
                drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraControlController::talk, "/camera/{1}/talk", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> status(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> presets(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> move(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> preset(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> settings(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> capabilities(drogon::HttpRequestPtr req, int64_t id);
  drogon::Task<drogon::HttpResponsePtr> talk(drogon::HttpRequestPtr req, int64_t id);

private:
  CameraControlFeatureService service_;
};
