#include "role-filter.hxx"

#include <config/app-config.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/enums.hxx>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{

struct RoleAccessRule
{
  std::string_view prefix;
  std::set<drogon::HttpMethod> methods;
};

using RoleAccessMap =
    std::unordered_map<UserRole, std::vector<RoleAccessRule>>;

const RoleAccessMap kRoleAccess = {
    {UserRole::Resident,
     {{"/camera", {drogon::Get, drogon::Post, drogon::Patch}},
      {"/zone", {drogon::Get, drogon::Post, drogon::Patch}},
      {"/reminder", {drogon::Get, drogon::Post, drogon::Patch}},
      {"/event", {drogon::Get, drogon::Post, drogon::Patch}},
      {"/person", {drogon::Get, drogon::Post, drogon::Patch}},
      {"/context-note", {drogon::Get, drogon::Post, drogon::Patch}},
      {"/auth", {drogon::Get, drogon::Post, drogon::Patch}}}},
    {UserRole::Guard,
     {{"/camera", {drogon::Get}},
      {"/event", {drogon::Get}},
      {"/person", {drogon::Get}},
      {"/zone", {drogon::Get}},
      {"/auth", {drogon::Get}}}},
    {UserRole::Guest,
     {{"/camera", {drogon::Get}}, {"/auth", {drogon::Get}}}},
};

bool isAllowed(UserRole role, drogon::HttpMethod method,
               const std::string& path)
{
  if (role == UserRole::Owner) return true;

  const auto it = kRoleAccess.find(role);
  if (it == kRoleAccess.end()) return false;

  for (const auto& rule : it->second) {
    if (path.starts_with(rule.prefix) &&
        rule.methods.contains(method))
      return true;
  }

  return false;
}

} // namespace

drogon::Task<drogon::HttpResponsePtr>
RoleFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  if (!req->getAttributes()->find(AppConfig::JWT_CTX_KEY)) {
    co_return AppConfig::get401Response();
  }

  const auto& ctx =
      req->getAttributes()->get<JwtContext>(AppConfig::JWT_CTX_KEY);

  if (!isAllowed(ctx.role, req->method(), req->getPath())) {
    co_return AppConfig::get403Response();
  }

  co_return drogon::HttpResponsePtr{};
}
