#include "remote-gate.hxx"

#include <config/app-config.hxx>
#include <drogon/drogon.h>
#include <filter/device/device-filter.hxx>

#include <chrono>
#include <exception>
#include <string>

namespace
{
bool isRemoteRestrictedPath(const std::string& path)
{
  return path == "/pairing" || path == "/auth/register";
}

bool isRateLimitedRoute(const drogon::HttpRequestPtr& req)
{
  return req->method() == drogon::Patch
         && req->path() == "/auth/refresh-token";
}

std::chrono::steady_clock::time_point now()
{
  return std::chrono::steady_clock::now();
}
} // namespace

RemoteGate::RemoteGate(RemoteConfig config,
                       std::shared_ptr<RefreshRateLimiter> limiter)
    : config_(config), limiter_(std::move(limiter))
{
}

drogon::HttpResponsePtr RemoteGate::check(const drogon::HttpRequestPtr& req,
                                          bool remote)
{
  if (remote)
    req->getAttributes()->insert(AppConfig::REMOTE_CTX_KEY, true);

  // Short-circuits bypass the post-handling advice, so CORS goes on here.
  if (limiter_ && limiter_->enabled() && isRateLimitedRoute(req)
      && !limiter_->admit(rateLimitKey(req), now())) {
    auto resp = AppConfig::get429Response();
    AppConfig::applyCors(resp);
    return resp;
  }

  if (remote && !config_.enabled && isRemoteRestrictedPath(req->path())) {
    auto resp = AppConfig::getRemoteNotAllowedResponse();
    AppConfig::applyCors(resp);
    return resp;
  }

  return nullptr;
}

void RemoteGate::recordOutcome(const drogon::HttpRequestPtr& req,
                               const drogon::HttpResponsePtr& resp)
{
  if (!limiter_ || !limiter_->enabled() || !isRateLimitedRoute(req))
    return;
  const bool success = resp->getStatusCode() < drogon::k400BadRequest;
  if (limiter_->recordResult({.key = rateLimitKey(req),
                              .success = success,
                              .now = now()}))
    LOG_WARN << "Refresh-token key locked out after consecutive failures";
}

std::string RemoteGate::rateLimitKey(const drogon::HttpRequestPtr& req)
{
  // The same fingerprint hash DeviceFilter stores; the peer IP bounds the limiter.
  try {
    return DeviceFilter::deviceKey(req);
  }
  catch (const std::exception&) {
    return "ip:" + req->getPeerAddr().toIp();
  }
}
