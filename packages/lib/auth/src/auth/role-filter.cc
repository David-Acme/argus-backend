#include "role-filter.hxx"

#include <auth/auth-errors.hxx>
#include <errors/response-exception.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <auth/role-access.hxx>

drogon::Task<drogon::HttpResponsePtr>
RoleFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  if (!req->getAttributes()->find(AuthContext::kJwtKey)) {
    throw ResponseException(AuthErrors::AuthenticationRequired);
  }

  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AuthContext::kJwtKey);

  if (!role_access::hasHttpAccess(
          {.role = ctx.role,
           .path = req->getPath(),
           .method = req->method()})) {
    throw ResponseException(AuthErrors::AccessDenied);
  }

  co_return drogon::HttpResponsePtr{};
}
