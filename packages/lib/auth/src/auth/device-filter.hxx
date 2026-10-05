#pragma once

#include <auth/session-origin.hxx>
#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>
#include <string>
#include <string_view>

struct DeviceContext
{
  std::string deviceHash;
  std::string userAgent;
  std::string ip;
  SessionOrigin origin{SessionOrigin::Unknown};
};

struct OriginInput
{
  bool viaTunnel{false};
  std::string_view address;
  std::string_view lanNetworks;
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

  static SessionOrigin resolveOrigin(const drogon::HttpRequestPtr& req,
                                     const std::string& address);

  static SessionOrigin classifyOrigin(const OriginInput& input);

  static constexpr std::string_view kDefaultLanNetworks =
      "10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,169.254.0.0/16,fc00::/7,"
      "fe80::/10";

private:
  static std::string hashFingerprint(const std::string& ua,
                                     const std::string& ip);
};
