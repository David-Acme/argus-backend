#pragma once

#include <array>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <feature/rate-gate/infra/rate-limiter.hxx>
#include <string>
#include <string_view>

struct IdentityGuardedRoute
{
  drogon::HttpMethod method{drogon::Post};
  std::string_view path;
  std::string_view name;
};

inline constexpr std::array<IdentityGuardedRoute, 2> kIdentityGuardedRoutes{{
    {.method = drogon::Post, .path = "/pairing", .name = "pairing"},
    {.method = drogon::Post, .path = "/invitation/resolve", .name = "invitation-resolve"},
}};

class IdentityRateGate
{
public:
  explicit IdentityRateGate(IdentityRateLimitConfig config);

  [[nodiscard]] drogon::HttpResponsePtr check(const drogon::HttpRequestPtr& req);
  void recordOutcome(const drogon::HttpRequestPtr& req,
                     const drogon::HttpResponsePtr& resp);

  [[nodiscard]] static std::string_view guardedRoute(const drogon::HttpRequestPtr& req);

private:
  [[nodiscard]] static std::string keyOf(const drogon::HttpRequestPtr& req,
                                         std::string_view route);

  RateLimiter limiter_;
};
