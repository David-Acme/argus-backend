#include "project-member-controller.hxx"

#include <errors/response-exception.hxx>
#include <feature/api/project-member/dtos/create-project-member-dto.hxx>
#include <feature/api/project-member/dtos/update-project-member-dto.hxx>
#include <auth/jwt-filter.hxx>
#include <http/api-response.hxx>
#include <membership-error.hxx>
#include <productivity-errors.hxx>
#include <request-context.hxx>

namespace
{
drogon::HttpResponsePtr failureFor(MembershipError error)
{
  switch (error) {
    case MembershipError::UserNotFound:
      throw ResponseException(ProductivityErrors::UserNotFound);
    case MembershipError::UserNotAllowed:
      throw ResponseException(ProductivityErrors::CannotSeeProjects);
    case MembershipError::SelfShare:
      throw ResponseException(ProductivityErrors::OwnerAlreadyHasAccess);
    default:
      throw ResponseException(ProductivityErrors::ProjectNotFound);
  }
}
} // namespace

drogon::Task<drogon::HttpResponsePtr>
ProjectMemberController::create(drogon::HttpRequestPtr req)
{
  const auto body = CreateProjectMemberDto::fromJson(*req->getJsonObject());
  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

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
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

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
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  if (!co_await service_.remove(id, ctx.sub))
    throw ResponseException(ProductivityErrors::ShareNotFound);

  Json::Value result;
  result["deleted"] = true;
  co_return ApiResponse::ok(result);
}
