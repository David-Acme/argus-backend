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

struct AddressFingerprintInput
{
  const std::string& userAgent;
  const std::string& address;
};

class DeviceFilter : public drogon::HttpCoroFilter<DeviceFilter, false>
{
public:
  drogon::Task<drogon::HttpResponsePtr>
  doFilter(const drogon::HttpRequestPtr& req) override;

  static std::string deviceKey(const drogon::HttpRequestPtr& req);

  static std::string sha256Hex(const std::string& data);

  static std::string credentialFingerprint(const std::string& ua,
                                           const std::string& secretHash);

  static std::string addressFingerprint(const AddressFingerprintInput& input);

  static bool credentialMode();

  static std::string resolveIp(const drogon::HttpRequestPtr& req);

private:
  static std::string hashFingerprint(const std::string& ua,
                                     const std::string& ip);
};
