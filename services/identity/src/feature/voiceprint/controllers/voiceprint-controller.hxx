#pragma once

#include <cstdint>
#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/services/voiceprint/voiceprint-feature-service.hxx>

class VoiceprintController
    : public drogon::HttpController<VoiceprintController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(VoiceprintController::directory, "/voiceprint/users",
                drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VoiceprintController::forget, "/voiceprint/user/{1}",
                drogon::Delete, "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> directory(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> forget(drogon::HttpRequestPtr req,
                                               int64_t userId);

private:
  VoiceprintFeatureService service_;
};
