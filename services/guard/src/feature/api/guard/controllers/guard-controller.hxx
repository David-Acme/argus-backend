#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/guard/services/guard-feature-service.hxx>

// Owner-only guard API; /guard is outside the sync table map, so RoleFilter
// admits Owner and denies every other role.
class GuardController : public drogon::HttpController<GuardController, false>
{
public:
  explicit GuardController(IdentityClient* identity);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(GuardController::mode, "/guard/mode", drogon::Get,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
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
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> mode(drogon::HttpRequestPtr req);
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

private:
  GuardFeatureService service_;
};
