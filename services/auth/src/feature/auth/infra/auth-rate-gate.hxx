#pragma once

#include <chrono>
#include <config/auth-config.hxx>
#include <deque>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class AuthRateGate
{
public:
  using SessionOfRefreshToken = std::function<std::string(const std::string&)>;

  explicit AuthRateGate(AuthRateLimitConfig config,
                        SessionOfRefreshToken sessionOf = {});

  [[nodiscard]] drogon::HttpResponsePtr
  check(const drogon::HttpRequestPtr& req);

  void recordOutcome(const drogon::HttpRequestPtr& req,
                     const drogon::HttpResponsePtr& resp);

  static constexpr int kPeerCeilingFactor = 20;
  static constexpr int kIpv6GroupBits = 64;

private:
  struct Entry
  {
    std::deque<std::chrono::steady_clock::time_point> hits;
    int consecutiveFailures{0};
    std::chrono::steady_clock::time_point lockedUntil{};
  };

  struct GateKey
  {
    std::string key;
    int maxRequests{0};
    int lockoutThreshold{0};
  };

  [[nodiscard]] bool enabled() const;
  [[nodiscard]] static std::string guardedRoute(const drogon::HttpRequestPtr& req);
  [[nodiscard]] std::vector<GateKey>
  gateKeys(const drogon::HttpRequestPtr& req) const;
  [[nodiscard]] std::string sessionOf(const drogon::HttpRequestPtr& req) const;
  [[nodiscard]] static std::string clientKey(const std::string& address);
  [[nodiscard]] bool admit(const GateKey& gateKey,
                           std::chrono::steady_clock::time_point now);
  void recordSuccess(const std::string& key);
  [[nodiscard]] bool recordFailure(const GateKey& gateKey,
                                   std::chrono::steady_clock::time_point now);
  void pruneExpired(std::chrono::steady_clock::time_point now);
  [[nodiscard]] bool makeRoom(std::chrono::steady_clock::time_point now);

  AuthRateLimitConfig config_;
  SessionOfRefreshToken sessionOf_;
  std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
};
