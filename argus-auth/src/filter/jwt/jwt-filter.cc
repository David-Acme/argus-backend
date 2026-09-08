#include "jwt-filter.hxx"

#include <config/app-config.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/identity-access.hxx>
#include <identity/identity-client.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <trantor/utils/Logger.h>

// One server-authoritative validation per request: user status, refresh-token
// row and device binding in a single round trip, fail closed when the identity
// service is unreachable.
drogon::Task<drogon::HttpResponsePtr>
JwtFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  const auto token = extractToken(req);
  if (token.empty()) {
    co_return AppConfig::get401Response("Missing authorization token");
  }

  const auto claims = jwtService_.verifyAccess(token);
  if (claims.empty()) {
    co_return AppConfig::get401Response();
  }

  const auto subIt = claims.find("sub");
  if (subIt == claims.end()) {
    co_return AppConfig::get401Response();
  }

  int64_t userId = 0;
  try {
    userId = std::stoll(subIt->second);
  }
  catch (const std::exception&) {
    co_return AppConfig::get401Response();
  }
  if (userId <= 0) {
    co_return AppConfig::get401Response();
  }

  const bool hasDeviceContext =
      req->getAttributes()->find(AppConfig::DEVICE_CTX_KEY);
  std::string deviceHash;
  if (hasDeviceContext) {
    deviceHash = req->getAttributes()
                     ->get<DeviceContext>(AppConfig::DEVICE_CTX_KEY)
                     .deviceHash;
  }

  const auto client = filterIdentityClient();
  const auto verdict =
      co_await BlockingTask<
          std::optional<argus::identity::v1::ValidateTokenResponse>>(
          [client, token, deviceHash, hasDeviceContext]() {
            return client->validateToken({token, deviceHash,
                                          hasDeviceContext});
          });

  if (!verdict || !verdict->valid()) {
    if (verdict && !verdict->reason().empty()) {
      co_return AppConfig::get401Response(verdict->reason());
    }
    co_return AppConfig::get401Response();
  }

  const auto& user = verdict->user();
  JwtContext ctx;
  ctx.sub = user.user_id();
  ctx.name = user.name() + " " + user.last_name();
  ctx.role = userRoleFromString(user.role());
  ctx.isActive = user.is_active();
  ctx.deviceHash = deviceHash;

  req->getAttributes()->insert(AppConfig::JWT_CTX_KEY, ctx);
  co_return drogon::HttpResponsePtr{};
}

std::string JwtFilter::extractToken(const drogon::HttpRequestPtr& req)
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
