#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <memory>
#include <server/refresh-rate-limiter.hxx>
#include <server/remote-config.hxx>

// Rulings CG/CJ: pre-routing gate for remote, LAN-only paths and rate limits.
class RemoteGate
{
public:
  RemoteGate(RemoteConfig config,
             std::shared_ptr<RefreshRateLimiter> limiter);

  // Returns a CORS-ready response only when the request must short-circuit.
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
