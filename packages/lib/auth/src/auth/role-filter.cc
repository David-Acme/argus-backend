#include "role-filter.hxx"

#include <auth/auth-errors.hxx>
#include <errors/response-exception.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/module-gate.hxx>
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

  const auto modules = moduleGate().current();
  switch (role_access::routeVerdict({.role = ctx.role,
                                     .path = req->getPath(),
                                     .method = req->method(),
                                     .modules = *modules})) {
    case role_access::RouteVerdict::Allowed:
      break;
    case role_access::RouteVerdict::ModuleDisabled:
      throw ResponseException(AuthErrors::ModuleDisabled);
    case role_access::RouteVerdict::RoleInactive:
      throw ResponseException(AuthErrors::RoleInactive);
    case role_access::RouteVerdict::Denied:
      throw ResponseException(AuthErrors::AccessDenied);
  }

  co_return drogon::HttpResponsePtr{};
}
