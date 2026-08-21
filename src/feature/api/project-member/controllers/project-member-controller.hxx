#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/project-member/services/project-member-feature-service.hxx>

class ProjectMemberController : public drogon::HttpController<ProjectMemberController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ProjectMemberController::create, "/project-member", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProjectMemberController::update, "/project-member/{1}", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProjectMemberController::remove, "/project-member/{1}", drogon::Delete,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> create(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> update(drogon::HttpRequestPtr req,
                                               int64_t id);
  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req,
                                               int64_t id);

private:
  ProjectMemberFeatureService service_;
};
