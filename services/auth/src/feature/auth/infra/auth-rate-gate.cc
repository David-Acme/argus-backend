#include "auth-rate-gate.hxx"

#include <algorithm>
#include <array>
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

struct GuardedRoute
{
  drogon::HttpMethod method;
  std::string_view path;
  std::string_view name;
};

constexpr std::array<GuardedRoute, 4> kGuardedRoutes = {{
    {.method = drogon::Patch, .path = "/auth/refresh-token", .name = "refresh"},
    {.method = drogon::Post, .path = "/auth/login", .name = "login"},
    {.method = drogon::Post, .path = "/auth/register", .name = "register"},
    {.method = drogon::Post, .path = "/auth/device-login", .name = "device-login"},
}};

bool samePath(std::string_view lhs, std::string_view rhs)
{
  return std::ranges::equal(lhs, rhs, [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

std::chrono::steady_clock::time_point now()
{
  return std::chrono::steady_clock::now();
}
}

AuthRateGate::AuthRateGate(AuthRateLimitConfig config) : config_(config)
{
}

bool AuthRateGate::enabled() const
{
  return config_.enabled;
}

std::string AuthRateGate::guardedRoute(const drogon::HttpRequestPtr& req)
{
  for (const auto& route : kGuardedRoutes) {
    if (req->method() == route.method && samePath(req->path(), route.path))
      return std::string(route.name);
  }
  return {};
}

std::string AuthRateGate::rateLimitKey(const drogon::HttpRequestPtr& req)
{
  return guardedRoute(req) + "|" + DeviceFilter::resolveIp(req);
}

drogon::HttpResponsePtr
AuthRateGate::check(const drogon::HttpRequestPtr& req)
{
  if (!enabled() || guardedRoute(req).empty())
    return nullptr;
  if (admit(rateLimitKey(req), now()))
    return nullptr;
  LOG_WARN << "Auth rate limit refused a " << guardedRoute(req) << " request";
  auto response = ApiResponse::error(AuthErrors::TooManyAttempts);
  Cors::apply(response);
  return response;
}

void AuthRateGate::recordOutcome(const drogon::HttpRequestPtr& req,
                                    const drogon::HttpResponsePtr& resp)
{
  if (!enabled() || guardedRoute(req).empty())
    return;
  const std::string key = rateLimitKey(req);
  if (resp->getStatusCode() < drogon::k400BadRequest) {
    recordSuccess(key);
    return;
  }
  if (recordFailure(key, now()))
    LOG_WARN << "Auth rate limit locked a " << guardedRoute(req)
             << " key out after consecutive failures";
}

bool AuthRateGate::admit(const std::string& key,
                            std::chrono::steady_clock::time_point now)
{
  const std::scoped_lock lock(mutex_);
  const auto windowStart = now - std::chrono::seconds(config_.windowSeconds);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    if (entries_.size() >= kMaxTrackedKeys && !makeRoom(now))
      return false;
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

void AuthRateGate::recordSuccess(const std::string& key)
{
  const std::scoped_lock lock(mutex_);
  const auto it = entries_.find(key);
  if (it == entries_.end())
    return;
  it->second.consecutiveFailures = 0;
}

bool AuthRateGate::recordFailure(const std::string& key,
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

void AuthRateGate::pruneExpired(std::chrono::steady_clock::time_point now)
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

bool AuthRateGate::makeRoom(std::chrono::steady_clock::time_point now)
{
  pruneExpired(now);
  if (entries_.size() < kMaxTrackedKeys)
    return true;
  const auto unlocked = std::ranges::find_if(entries_, [now](const auto& entry) {
    return !(now < entry.second.lockedUntil);
  });
  if (unlocked == entries_.end())
    return false;
  entries_.erase(unlocked);
  return true;
}
