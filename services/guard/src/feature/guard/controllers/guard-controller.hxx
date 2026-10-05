#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/guard/services/guard-feature-service.hxx>

class GuardController : public drogon::HttpController<GuardController, false>
{
public:
  explicit GuardController(const GuardFeatureDependencies& dependencies);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(GuardController::setMode, "/guard/mode", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(GuardController::incidents, "/guard/incidents", drogon::Get,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(GuardController::decisions, "/guard/decisions", drogon::Get,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(GuardController::decisionsSummary, "/guard/decisions/summary",
                drogon::Get, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::feedback, "/guard/decisions/{1}/feedback",
                drogon::Post, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::createGuest, "/guard/expected-guests",
                drogon::Post, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::listGuests, "/guard/expected-guests",
                drogon::Get, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::removeGuest, "/guard/expected-guests",
                drogon::Delete, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::promotePerson, "/guard/person/{1}/promote",
                drogon::Post, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::environments, "/guard/environments",
                drogon::Get, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::createEnvironment, "/guard/environments",
                drogon::Post, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::updateEnvironment, "/guard/environments/{1}",
                drogon::Patch, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::removeEnvironment, "/guard/environments/{1}",
                drogon::Delete, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::cameras, "/guard/cameras", drogon::Get,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(GuardController::setCamera, "/guard/cameras/{1}", drogon::Put,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(GuardController::episodes, "/guard/episodes", drogon::Get,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(GuardController::episode, "/guard/episodes/{1}", drogon::Get,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(GuardController::reviewEpisode, "/guard/episodes/{1}/review",
                drogon::Post, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(GuardController::retainEpisode, "/guard/episodes/{1}/retain",
                drogon::Post, "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> setMode(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> incidents(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> decisions(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> decisionsSummary(
      drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> feedback(drogon::HttpRequestPtr req,
                                                 const std::string& eventId);
  drogon::Task<drogon::HttpResponsePtr> createGuest(
      drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> listGuests(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> removeGuest(
      drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> promotePerson(
      drogon::HttpRequestPtr req, int64_t personId);
  drogon::Task<drogon::HttpResponsePtr> environments(
      drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> createEnvironment(
      drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> updateEnvironment(
      drogon::HttpRequestPtr req, int64_t environmentId);
  drogon::Task<drogon::HttpResponsePtr> removeEnvironment(
      drogon::HttpRequestPtr req, int64_t environmentId);
  drogon::Task<drogon::HttpResponsePtr> cameras(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> setCamera(drogon::HttpRequestPtr req,
                                                  int64_t cameraId);
  drogon::Task<drogon::HttpResponsePtr> episodes(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> episode(drogon::HttpRequestPtr req,
                                                int64_t episodeId);
  drogon::Task<drogon::HttpResponsePtr> retainEpisode(drogon::HttpRequestPtr req,
                                                      int64_t episodeId);
  drogon::Task<drogon::HttpResponsePtr> reviewEpisode(
      drogon::HttpRequestPtr req, int64_t episodeId);

private:
  GuardFeatureService service_;
};
