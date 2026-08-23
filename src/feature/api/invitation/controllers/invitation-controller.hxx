#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/invitation/services/invitation-feature-service.hxx>

class InvitationController : public drogon::HttpController<InvitationController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(InvitationController::resolve, "/invitation/resolve",
                drogon::Post, "ValidJsonFilter");
  ADD_METHOD_TO(InvitationController::list, "/invitation", drogon::Get,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(InvitationController::create, "/invitation", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter",
                "RoleFilter");
  ADD_METHOD_TO(InvitationController::revoke, "/invitation/{1}",
                drogon::Delete, "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> resolve(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> list(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> create(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr>
  revoke(drogon::HttpRequestPtr req, int64_t invitationId);

private:
  InvitationFeatureService service_;
};
