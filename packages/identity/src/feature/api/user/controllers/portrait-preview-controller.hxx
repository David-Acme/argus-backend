#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/user/services/portrait-preview-service.hxx>

class PortraitPreviewController
    : public drogon::HttpController<PortraitPreviewController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(PortraitPreviewController::create, "/portrait-preview/{1}",
                drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(PortraitPreviewController::consume,
                "/portrait-preview/{1}/content", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr>
  create(drogon::HttpRequestPtr req, int64_t portraitUserId);
  drogon::Task<drogon::HttpResponsePtr>
  consume(drogon::HttpRequestPtr req, std::string token);

private:
  PortraitPreviewService service_;
};
