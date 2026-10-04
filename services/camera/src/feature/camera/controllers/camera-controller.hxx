#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/camera/services/camera-feature-service.hxx>
#include <feature/camera/services/camera-overview-service.hxx>
#include <feature/camera/services/camera-probe-service.hxx>

class CameraController : public drogon::HttpController<CameraController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(CameraController::catalog, "/camera/catalog", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraController::overview, "/camera/overview", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraController::probe, "/camera/probe", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraController::create, "/camera", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraController::update, "/camera/{1}", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(CameraController::remove, "/camera/{1}", drogon::Delete,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> catalog(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> overview(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> probe(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> create(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> update(drogon::HttpRequestPtr req,
                                               int64_t id);
  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req,
                                               int64_t id);

private:
  CameraFeatureService cameraService_;
  CameraProbeService probeService_;
  CameraOverviewService overviewService_;
};
