#include "role-filter.hxx"

#include <config/app-config.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/access/role-access.hxx>

drogon::Task<drogon::HttpResponsePtr>
RoleFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  if (!req->getAttributes()->find(AppConfig::JWT_CTX_KEY)) {
    co_return AppConfig::get401Response();
  }

  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  if (!role_access::hasHttpAccess(ctx.role, req->getPath(), req->method())) {
    co_return AppConfig::get403Response();
  }

  co_return drogon::HttpResponsePtr{};
}
