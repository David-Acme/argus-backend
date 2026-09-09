#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <memory>
#include <server/refresh-rate-limiter.hxx>
#include <server/remote-config.hxx>

// Rulings CG/CJ: one pre-routing gate classifies tunnel-listener requests as
// remote, keeps /pairing and /auth/register LAN-only behind [remote].enabled
// and rate-limits PATCH /auth/refresh-token before routing, filters and any
// database access.
class RemoteGate
{
public:
  RemoteGate(RemoteConfig config,
             std::shared_ptr<RefreshRateLimiter> limiter);

  // Marks remote requests with the remote_ctx attribute and returns a
  // non-null CORS-ready response only when the request must short-circuit.
  drogon::HttpResponsePtr check(const drogon::HttpRequestPtr& req,
                                bool remote);
  // Feeds the handler outcome into the lockout counter (post-handling).
  void recordOutcome(const drogon::HttpRequestPtr& req,
                     const drogon::HttpResponsePtr& resp);

private:
  std::string rateLimitKey(const drogon::HttpRequestPtr& req);

  RemoteConfig config_;
  std::shared_ptr<RefreshRateLimiter> limiter_;
};
