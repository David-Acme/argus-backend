#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>
#include <shared/repositories/device-credential/device-credential-repository.hxx>
#include <string>

struct DeviceContext
{
  std::string deviceHash;
  std::string userAgent;
  std::string ip;
};

class DeviceFilter : public drogon::HttpCoroFilter<DeviceFilter, false>
{
public:
  drogon::Task<drogon::HttpResponsePtr>
  doFilter(const drogon::HttpRequestPtr& req) override;

  // Same fingerprint hash doFilter stores for this request; throws when no
  // fingerprint secret is configured.
  static std::string deviceKey(const drogon::HttpRequestPtr& req);

  // SHA-256 hex of a device credential plaintext.
  static std::string sha256Hex(const std::string& data);

  // Credential-mode fingerprint: HMAC over ua|secretHash, source IP absent.
  static std::string credentialFingerprint(const std::string& ua,
                                           const std::string& secretHash);

private:
  static std::string hashFingerprint(const std::string& ua,
                                     const std::string& ip);
  static std::string resolveIp(const drogon::HttpRequestPtr& req);
  static bool credentialMode();

  DeviceCredentialRepository repository_;
};
