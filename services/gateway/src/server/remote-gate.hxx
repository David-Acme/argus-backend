#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <memory>
#include <server/refresh-rate-limiter.hxx>
#include <server/remote-config.hxx>
#include <string>

class RemoteGate
{
public:
  static inline const std::string kRemoteContextKey{"remote_ctx"};

  RemoteGate(RemoteConfig config,
             std::shared_ptr<RefreshRateLimiter> limiter);

  drogon::HttpResponsePtr check(const drogon::HttpRequestPtr& req,
                                bool remote);
  void recordOutcome(const drogon::HttpRequestPtr& req,
                     const drogon::HttpResponsePtr& resp);

private:
  std::string rateLimitKey(const drogon::HttpRequestPtr& req);

  RemoteConfig config_;
  std::shared_ptr<RefreshRateLimiter> limiter_;
};
