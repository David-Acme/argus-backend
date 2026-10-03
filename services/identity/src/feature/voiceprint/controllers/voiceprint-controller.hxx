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
  ADD_METHOD_TO(VoiceprintController::status, "/voiceprint/me", drogon::Get,
                "DeviceFilter", "JwtFilter");
  ADD_METHOD_TO(VoiceprintController::challenge, "/voiceprint/me/challenge",
                drogon::Post, "DeviceFilter", "ValidJsonFilter", "JwtFilter");
  ADD_METHOD_TO(VoiceprintController::checkSample, "/voiceprint/me/sample",
                drogon::Post, "DeviceFilter", "JwtFilter");
  ADD_METHOD_TO(VoiceprintController::enroll, "/voiceprint/me", drogon::Post,
                "DeviceFilter", "JwtFilter");
  ADD_METHOD_TO(VoiceprintController::verify, "/voiceprint/me/verify",
                drogon::Post, "DeviceFilter", "JwtFilter");
  ADD_METHOD_TO(VoiceprintController::remove, "/voiceprint/me", drogon::Delete,
                "DeviceFilter", "JwtFilter");
  ADD_METHOD_TO(VoiceprintController::statusOf, "/voiceprint/user/{1}",
                drogon::Get, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VoiceprintController::challengeFor,
                "/voiceprint/user/{1}/challenge", drogon::Post, "DeviceFilter",
                "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VoiceprintController::enrollFor, "/voiceprint/user/{1}",
                drogon::Post, "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(VoiceprintController::removeFor, "/voiceprint/user/{1}",
                drogon::Delete, "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> status(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> challenge(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> checkSample(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> enroll(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> verify(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> statusOf(drogon::HttpRequestPtr req,
                                                 int64_t userId);
  drogon::Task<drogon::HttpResponsePtr> challengeFor(drogon::HttpRequestPtr req,
                                                     int64_t userId);
  drogon::Task<drogon::HttpResponsePtr> enrollFor(drogon::HttpRequestPtr req,
                                                  int64_t userId);
  drogon::Task<drogon::HttpResponsePtr> removeFor(drogon::HttpRequestPtr req,
                                                  int64_t userId);

private:
  VoiceprintFeatureService service_;
};
