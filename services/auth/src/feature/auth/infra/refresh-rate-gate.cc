#include "refresh-rate-gate.hxx"

#include <algorithm>
#include <auth/auth-errors.hxx>
#include <auth/device-filter.hxx>
#include <cctype>
#include <cstddef>
#include <exception>
#include <http/api-response.hxx>
#include <http/cors.hxx>
#include <string_view>
#include <trantor/utils/Logger.h>

namespace
{
constexpr std::size_t kMaxTrackedKeys = 4096;
constexpr std::string_view kGuardedPath = "/auth/refresh-token";

std::chrono::steady_clock::time_point now()
{
  return std::chrono::steady_clock::now();
}
}

RefreshRateGate::RefreshRateGate(AuthRateLimitConfig config) : config_(config)
{
}

bool RefreshRateGate::enabled() const
{
  return config_.enabled;
}

bool RefreshRateGate::isGuardedRoute(const drogon::HttpRequestPtr& req)
{
  if (req->method() != drogon::Patch)
    return false;
  return std::ranges::equal(req->path(), kGuardedPath, [](char lhs, char rhs) {
    return std::tolower(static_cast<unsigned char>(lhs)) ==
           std::tolower(static_cast<unsigned char>(rhs));
  });
}

std::string RefreshRateGate::rateLimitKey(const drogon::HttpRequestPtr& req)
{
  try {
    return DeviceFilter::deviceKey(req);
  }
  catch (const std::exception&) {
    return "ip:" + req->getPeerAddr().toIp();
  }
}

drogon::HttpResponsePtr
RefreshRateGate::check(const drogon::HttpRequestPtr& req)
{
  if (!enabled() || !isGuardedRoute(req))
    return nullptr;
  if (admit(rateLimitKey(req), now()))
    return nullptr;
  LOG_WARN << "Refresh-token rate limit refused a request";
  auto response = ApiResponse::error(AuthErrors::TooManyAttempts);
  Cors::apply(response);
  return response;
}

void RefreshRateGate::recordOutcome(const drogon::HttpRequestPtr& req,
                                    const drogon::HttpResponsePtr& resp)
{
  if (!enabled() || !isGuardedRoute(req))
    return;
  const std::string key = rateLimitKey(req);
  if (resp->getStatusCode() < drogon::k400BadRequest) {
    recordSuccess(key);
    return;
  }
  if (recordFailure(key, now()))
    LOG_WARN << "Refresh-token key locked out after consecutive failures";
}

bool RefreshRateGate::admit(const std::string& key,
                            std::chrono::steady_clock::time_point now)
{
  const std::scoped_lock lock(mutex_);
  const auto windowStart = now - std::chrono::seconds(config_.windowSeconds);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    if (entries_.size() >= kMaxTrackedKeys) {
      pruneExpired(now);
      if (entries_.size() >= kMaxTrackedKeys)
        return false;
    }
    it = entries_.emplace(key, Entry{}).first;
  }
  if (now < it->second.lockedUntil)
    return false;
  while (!it->second.hits.empty() && it->second.hits.front() < windowStart)
    it->second.hits.pop_front();
  if (static_cast<int>(it->second.hits.size()) >= config_.maxRequests)
    return false;
  it->second.hits.push_back(now);
  return true;
}

void RefreshRateGate::recordSuccess(const std::string& key)
{
  const std::scoped_lock lock(mutex_);
  const auto it = entries_.find(key);
  if (it == entries_.end())
    return;
  it->second.consecutiveFailures = 0;
}

bool RefreshRateGate::recordFailure(const std::string& key,
                                    std::chrono::steady_clock::time_point now)
{
  const std::scoped_lock lock(mutex_);
  const auto it = entries_.find(key);
  if (it == entries_.end())
    return false;
  if (now < it->second.lockedUntil)
    return false;
  ++it->second.consecutiveFailures;
  if (it->second.consecutiveFailures < config_.lockoutThreshold)
    return false;
  it->second.lockedUntil = now + std::chrono::seconds(config_.lockoutSeconds);
  return true;
}

void RefreshRateGate::pruneExpired(std::chrono::steady_clock::time_point now)
{
  const auto windowStart = now - std::chrono::seconds(config_.windowSeconds);
  for (auto it = entries_.begin(); it != entries_.end();) {
    const bool hitsExpired =
        it->second.hits.empty() || it->second.hits.front() < windowStart;
    const bool lockoutExpired = !(now < it->second.lockedUntil);
    if (hitsExpired && lockoutExpired)
      it = entries_.erase(it);
    else
      ++it;
  }
}
