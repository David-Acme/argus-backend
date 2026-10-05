#include "jwt-filter.hxx"

#include <algorithm>
#include <cctype>
#include <string_view>

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

  if (!req->getAttributes()->find(AuthContext::kDeviceKey))
    throw ResponseException(AuthErrors::DeviceContextMissing);
  const auto& device =
      req->getAttributes()->get<DeviceContext>(AuthContext::kDeviceKey);
  const std::string deviceHash = device.deviceHash;
  const std::string origin = device.origin == SessionOrigin::Unknown
                                 ? std::string{}
                                 : sessionOriginToString(device.origin);

  const auto client = filterAuthClient();
  const auto verdict = co_await BlockingTask<
      std::optional<argus::auth::v1::ValidateTokenResponse>>(
      [client, token, deviceHash, origin]() {
        return client->validateToken({.accessToken = token,
                                      .deviceHash = deviceHash,
                                      .hasDeviceContext = true,
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

bool JwtFilter::isWebSocketUpgrade(const drogon::HttpRequestPtr& req)
{
  const std::string& upgrade = req->getHeader("Upgrade");
  return std::ranges::equal(upgrade, std::string_view("websocket"),
                            [](char left, char right) {
                              return std::tolower(static_cast<unsigned char>(left)) == right;
                            });
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

  if (isWebSocketUpgrade(req))
    return req->getParameter("token");

  return {};
}
