#include "project-member-controller.hxx"

#include <config/app-config.hxx>
#include <feature/api/project-member/dtos/create-project-member-dto.hxx>
#include <feature/api/project-member/dtos/update-project-member-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

namespace
{
drogon::HttpResponsePtr failureFor(MembershipError error)
{
  switch (error) {
    case MembershipError::UserNotFound:
      return AppConfig::get404Response("User not found");
    case MembershipError::UserNotAllowed:
      return AppConfig::get403Response("That user cannot see projects");
    case MembershipError::SelfShare:
      return AppConfig::get409Response("The owner already has access");
    default:
      return AppConfig::get404Response("Project not found");
  }
}
} // namespace

drogon::Task<drogon::HttpResponsePtr>
ProjectMemberController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateProjectMemberDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  const auto result = co_await service_.create(body, ctx.sub);
  if (!result.row)
    co_return failureFor(result.error);
  co_return ApiResponse::ok(result.row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ProjectMemberController::update(drogon::HttpRequestPtr req, int64_t id)
{
  const auto body = UpdateProjectMemberDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  const auto result = co_await service_.update(
      {.id = id, .body = body, .actorId = ctx.sub});
  if (!result.row)
    co_return failureFor(result.error);
  co_return ApiResponse::ok(result.row->toJson());
}

drogon::Task<drogon::HttpResponsePtr>
ProjectMemberController::remove(drogon::HttpRequestPtr req, int64_t id)
{
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  if (!co_await service_.remove(id, ctx.sub))
    co_return AppConfig::get404Response("Share not found");

  Json::Value result;
  result["deleted"] = true;
  co_return ApiResponse::ok(result);
}
