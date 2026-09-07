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

  // Short-circuit responses bypass the post-handling advice, so CORS is
  // applied here to keep the response headers identical to every other
  // gateway response.
  if (limiter_ && isRateLimitedRoute(req)
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
  if (!limiter_ || !isRateLimitedRoute(req))
    return;
  const bool success = resp->getStatusCode() < drogon::k400BadRequest;
  if (limiter_->recordResult(rateLimitKey(req), success, now()))
    LOG_WARN << "Refresh-token key locked out after consecutive failures";
}

std::string RemoteGate::rateLimitKey(const drogon::HttpRequestPtr& req)
{
  // The refresh-token route is DeviceFilter'd, so this is the same
  // fingerprint hash the filter stores; the peer IP keeps the limiter
  // bounded when the fingerprint secret is unconfigured.
  try {
    return DeviceFilter::deviceKey(req);
  }
  catch (const std::exception&) {
    return "ip:" + req->getPeerAddr().toIp();
  }
}
