#include "identity-rate-gate.hxx"

#include <algorithm>
#include <auth/device-filter.hxx>
#include <cctype>
#include <chrono>
#include <errors/response-exception.hxx>
#include <http/api-response.hxx>
#include <http/cors.hxx>
#include <identity/identity-errors.hxx>
#include <trantor/utils/Logger.h>

namespace
{
constexpr int kIpv6GroupBits = 64;

bool samePath(std::string_view lhs, std::string_view rhs)
{
  return std::ranges::equal(lhs, rhs, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}
}

IdentityRateGate::IdentityRateGate(IdentityRateLimitConfig config) : limiter_(config) {}

std::string_view IdentityRateGate::guardedRoute(const drogon::HttpRequestPtr& req)
{
  for (const auto& route : kIdentityGuardedRoutes) {
    if (req->method() == route.method && samePath(req->path(), route.path))
      return route.name;
  }
  return {};
}

std::string IdentityRateGate::keyOf(const drogon::HttpRequestPtr& req,
                                    std::string_view route)
{
  return std::string(route) + "|" +
         DeviceFilter::networkPrefix({.address = DeviceFilter::resolveIp(req),
                                      .ipv4Bits = 32,
                                      .ipv6Bits = kIpv6GroupBits});
}

drogon::HttpResponsePtr IdentityRateGate::check(const drogon::HttpRequestPtr& req)
{
  const auto route = guardedRoute(req);
  if (route.empty())
    return nullptr;
  if (limiter_.admit({.key = keyOf(req, route), .now = std::chrono::steady_clock::now()}))
    return nullptr;
  LOG_WARN << "Identity rate limit refused a " << route << " request";
  auto response = ApiResponse::error(IdentityErrors::TooManyAttempts);
  Cors::apply(response);
  return response;
}

void IdentityRateGate::recordOutcome(const drogon::HttpRequestPtr& req,
                                     const drogon::HttpResponsePtr& resp)
{
  const auto route = guardedRoute(req);
  if (route.empty())
    return;
  const auto status = resp->getStatusCode();
  if (status == drogon::k429TooManyRequests)
    return;
  const auto key = keyOf(req, route);
  if (status < drogon::k400BadRequest) {
    limiter_.recordSuccess(key);
    return;
  }
  if (status < drogon::k500InternalServerError)
    limiter_.recordFailure({.key = key, .now = std::chrono::steady_clock::now()});
}
