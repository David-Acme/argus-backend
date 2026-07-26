#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>
#include <string>

struct DeviceContext
{
  std::string deviceHash;
  std::string userAgent;
  std::string ip;
};

class DeviceFilter : public drogon::HttpCoroFilter<DeviceFilter>
{
public:
  drogon::Task<drogon::HttpResponsePtr>
  doFilter(const drogon::HttpRequestPtr& req) override;

private:
  static std::string hashFingerprint(const std::string& ua,
                                     const std::string& ip);
  static std::string resolveIp(const drogon::HttpRequestPtr& req);
};
