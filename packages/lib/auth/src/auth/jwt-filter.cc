#include "jwt-filter.hxx"

#include <auth/auth-errors.hxx>
#include <auth/auth-client.hxx>
#include <errors/response-exception.hxx>
#include <auth/device-filter.hxx>
#include <auth/auth-access.hxx>
#include <auth/request-context.hxx>
#include <runtime/blocking-task.hxx>
#include <trantor/utils/Logger.h>

drogon::Task<drogon::HttpResponsePtr>
JwtFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  const auto token = extractToken(req);
  if (token.empty()) {
    throw ResponseException(AuthErrors::MissingToken);
  }

  const auto claims = jwtService_.verifyAccess(token);
  if (claims.empty()) {
    throw ResponseException(AuthErrors::AuthenticationRequired);
  }

  const auto subIt = claims.find("sub");
  if (subIt == claims.end()) {
    throw ResponseException(AuthErrors::AuthenticationRequired);
  }

  int64_t userId = 0;
  try {
    userId = std::stoll(subIt->second);
  }
  catch (const std::exception&) {
    throw ResponseException(AuthErrors::AuthenticationRequired);
  }
  if (userId <= 0) {
    throw ResponseException(AuthErrors::AuthenticationRequired);
  }

  const bool hasDeviceContext =
      req->getAttributes()->find(AuthContext::kDeviceKey);
  std::string deviceHash;
  std::string origin;
  if (hasDeviceContext) {
    const auto& device =
        req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
    deviceHash = device.deviceHash;
    if (device.origin != SessionOrigin::Unknown)
      origin = sessionOriginToString(device.origin);
  }

  const auto client = filterAuthClient();
  const auto verdict = co_await BlockingTask<
      std::optional<argus::auth::v1::ValidateTokenResponse>>(
      [client, token, deviceHash, hasDeviceContext, origin]() {
        return client->validateToken({.accessToken = token,
                                      .deviceHash = deviceHash,
                                      .hasDeviceContext = hasDeviceContext,
                                      .origin = origin});
      });

  if (!verdict)
    throw ResponseException(AuthErrors::AuthUnavailable);
  if (!verdict->valid()) {
    if (!verdict->reason().empty()) {
      throw ResponseException(AuthErrors::AuthenticationRequired
                                  .withMessage(verdict->reason()));
    }
    throw ResponseException(AuthErrors::AuthenticationRequired);
  }

  const auto& user = verdict->user();
  JwtContext ctx;
  ctx.sub = user.user_id();
  ctx.name = user.name() + " " + user.last_name();
  ctx.role = userRoleFromString(user.role());
  ctx.isActive = user.is_active();
  ctx.deviceHash = deviceHash;
  ctx.sessionId = verdict->session_id();

  req->getAttributes()->insert(AuthContext::kJwtKey, ctx);
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
