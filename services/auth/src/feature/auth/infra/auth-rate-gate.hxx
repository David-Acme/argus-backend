#pragma once

#include <chrono>
#include <config/auth-config.hxx>
#include <deque>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <mutex>
#include <string>
#include <unordered_map>

class AuthRateGate
{
public:
  explicit AuthRateGate(AuthRateLimitConfig config);

  [[nodiscard]] drogon::HttpResponsePtr
  check(const drogon::HttpRequestPtr& req);

  void recordOutcome(const drogon::HttpRequestPtr& req,
                     const drogon::HttpResponsePtr& resp);

private:
  struct Entry
  {
    std::deque<std::chrono::steady_clock::time_point> hits;
    int consecutiveFailures{0};
    std::chrono::steady_clock::time_point lockedUntil{};
  };

  [[nodiscard]] bool enabled() const;
  [[nodiscard]] static std::string guardedRoute(const drogon::HttpRequestPtr& req);
  [[nodiscard]] static std::string
  rateLimitKey(const drogon::HttpRequestPtr& req);
  [[nodiscard]] bool admit(const std::string& key,
                           std::chrono::steady_clock::time_point now);
  void recordSuccess(const std::string& key);
  [[nodiscard]] bool recordFailure(const std::string& key,
                                   std::chrono::steady_clock::time_point now);
  void pruneExpired(std::chrono::steady_clock::time_point now);
  [[nodiscard]] bool makeRoom(std::chrono::steady_clock::time_point now);

  AuthRateLimitConfig config_;
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
};
