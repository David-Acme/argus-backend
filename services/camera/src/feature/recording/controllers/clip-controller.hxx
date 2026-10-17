#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/recording/services/clip-service.hxx>

class ClipController : public drogon::HttpController<ClipController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ClipController::list, "/camera/{1}/clips", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ClipController::preview, "/camera/{1}/clips/{2}/preview",
                drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ClipController::remove, "/camera/{1}/clips/{2}", drogon::Delete,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ClipController::content, "/clip/{1}/content", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req,
                                             int64_t cameraId);
  drogon::Task<drogon::HttpResponsePtr> preview(drogon::HttpRequestPtr req,
                                                int64_t cameraId, int64_t clipId);
  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req,
                                               int64_t cameraId, int64_t clipId);
  drogon::Task<drogon::HttpResponsePtr> content(drogon::HttpRequestPtr req,
                                                std::string token);

private:
  ClipService service_;
};
