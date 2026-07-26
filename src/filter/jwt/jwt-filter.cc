#include "jwt-filter.hxx"

#include <config/app-config.hxx>
#include <filter/device/device-filter.hxx>
#include <shared/repositories/refresh-token/refresh-token-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/jwt/jwt-service.hxx>
#include <trantor/utils/Logger.h>

drogon::Task<drogon::HttpResponsePtr>
JwtFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  const auto token = extractToken(req);
  if (token.empty()) {
    co_return AppConfig::get401Response("Missing authorization token");
  }

  std::map<std::string, std::string> claims;
  try {
    claims = JwtService::verifyAccess(token);
  } catch (const std::exception& ex) {
    LOG_WARN << "JWT verification failed: " << ex.what();
    co_return AppConfig::get401Response();
  }

  const auto subIt = claims.find("sub");
  if (subIt == claims.end()) {
    co_return AppConfig::get401Response();
  }

  const int64_t userId = std::stoll(subIt->second);
  const auto user = co_await UserRepository().findById(userId);
  if (!user) {
    co_return AppConfig::get401Response();
  }

  if (!user->isActive) {
    co_return AppConfig::get401Response("User account is disabled");
  }

  if (req->getAttributes()->find(AppConfig::DEVICE_CTX_KEY)) {
    const auto& devCtx =
        req->getAttributes()->get<DeviceContext>(AppConfig::DEVICE_CTX_KEY);
    const auto rt =
        co_await RefreshTokenRepository().findByAccessToken(userId, token);
    if (!rt || !rt->isValid || rt->isUsed) {
      co_return AppConfig::get401Response();
    }
    if (rt->deviceHash != devCtx.deviceHash) {
      LOG_WARN << "Device hash mismatch for user " << userId;
      co_return AppConfig::get401Response();
    }
  }

  JwtContext ctx;
  ctx.userId = user->id;
  ctx.featureHubId = user->featureHubId;
  ctx.name = user->name + " " + user->lastName;
  ctx.role = user->role;
  ctx.isActive = user->isActive;

  req->getAttributes()->insert(AppConfig::JWT_CTX_KEY, ctx);
  co_return drogon::HttpResponsePtr{};
}

std::string JwtFilter::extractToken(
    const drogon::HttpRequestPtr& req) const
{
  const auto auth = req->getHeader("Authorization");
  if (!auth.empty()) {
    constexpr std::string_view prefix = "Bearer ";
    if (auth.size() > prefix.size() &&
        std::string_view(auth).substr(0, prefix.size()) == prefix) {
      return auth.substr(prefix.size());
    }
  }

  if (auto token = req->getParameter("token"); !token.empty()) {
    return token;
  }

  if (auto cookie = req->getCookie("authorization"); !cookie.empty()) {
    constexpr std::string_view prefix = "Bearer ";
    if (std::string_view(cookie).substr(0, prefix.size()) == prefix)
      return cookie.substr(prefix.size());
    return cookie;
  }

  return {};
}
