#pragma once

#include <cstdint>
#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/visitor/services/visitor-feature-service.hxx>
#include <string>

class VisitorController : public drogon::HttpController<VisitorController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(VisitorController::list, "/visitor", drogon::Get, "DeviceFilter",
                "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VisitorController::settings, "/visitor-settings", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VisitorController::updateSettings, "/visitor-settings",
                drogon::Patch, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(VisitorController::detail, "/visitor/{1}", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VisitorController::update, "/visitor/{1}", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VisitorController::remove, "/visitor/{1}", drogon::Delete,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VisitorController::merge, "/visitor/{1}/merge", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VisitorController::split, "/visitor/{1}/split", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VisitorController::removeSample, "/visitor/{1}/samples/{2}",
                drogon::Delete, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VisitorController::mintCrop, "/visitor/{1}/crop-preview",
                drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VisitorController::consumeCrop, "/visitor-crop/{1}/content",
                drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> settings(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> updateSettings(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> detail(drogon::HttpRequestPtr req,
                                               int64_t personId);
  drogon::Task<drogon::HttpResponsePtr> update(drogon::HttpRequestPtr req,
                                               int64_t personId);
  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req,
                                               int64_t personId);
  drogon::Task<drogon::HttpResponsePtr> merge(drogon::HttpRequestPtr req,
                                              int64_t personId);
  drogon::Task<drogon::HttpResponsePtr> split(drogon::HttpRequestPtr req,
                                              int64_t personId);
  drogon::Task<drogon::HttpResponsePtr>
  removeSample(drogon::HttpRequestPtr req, int64_t personId, int64_t sampleId);
  drogon::Task<drogon::HttpResponsePtr> mintCrop(drogon::HttpRequestPtr req,
                                                 int64_t personId);
  drogon::Task<drogon::HttpResponsePtr> consumeCrop(drogon::HttpRequestPtr req,
                                                    std::string token);

private:
  VisitorFeatureService service_;
};
