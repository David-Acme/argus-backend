#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/project/services/project-feature-service.hxx>

class ProjectController : public drogon::HttpController<ProjectController>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(ProjectController::create, "/project", drogon::Post,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProjectController::update, "/project/{1}", drogon::Patch,
                "DeviceFilter", "ValidJsonFilter", "JwtFilter", "RoleFilter");
  ADD_METHOD_TO(ProjectController::remove, "/project/{1}", drogon::Delete,
                "DeviceFilter", "JwtFilter", "RoleFilter");
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> create(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> update(drogon::HttpRequestPtr req,
                                               int64_t id);
  drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req,
                                               int64_t id);

private:
  ProjectFeatureService service_;
};
