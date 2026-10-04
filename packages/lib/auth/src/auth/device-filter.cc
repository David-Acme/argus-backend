#include "device-filter.hxx"

#include <auth/auth-client.hxx>
#include <auth/auth-access.hxx>
#include <auth/details/proxy-allowlist.hxx>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <auth/request-context.hxx>
#include <config/config-service.hxx>
#include <runtime/blocking-task.hxx>
#include <stdexcept>
#include <auth/auth-errors.hxx>
#include <errors/response-exception.hxx>
#include <optional>
#include <string_view>

namespace
{
constexpr size_t kMaxCredentialLength = 128;

bool trustedProxy(const std::string& peer)
{
  const std::string configured =
      ConfigService::getString("device.trusted_proxy_ips");
  return proxy_allowlist::contains({.configured = configured, .address = peer});
}

std::string fingerprintKey()
{
  auto key = ConfigService::getString("device.fingerprint_secret");
  if (key.empty())
    key = ConfigService::getString("jwt.secret");
  if (key.empty())
    throw std::runtime_error("Device fingerprint secret is not configured");
  return key;
}
}

drogon::Task<drogon::HttpResponsePtr>
DeviceFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  const auto ua = req->getHeader("User-Agent");
  const auto ip = resolveIp(req);

  DeviceContext ctx;
  ctx.userAgent = ua;
  ctx.ip = ip;
  if (credentialMode()) {
    const auto credential = req->getHeader("X-Argus-Device-Credential");
    std::string deviceHash;
    if (!credential.empty() && credential.size() <= kMaxCredentialLength) {
      const auto secretHash = sha256Hex(credential);
      const auto client = filterAuthClient();
      const auto active = co_await BlockingTask<std::optional<bool>>(
          [client, secretHash]() {
            return client->checkDeviceCredential(secretHash);
          });
      if (!active)
        throw ResponseException(AuthErrors::AuthUnavailable);
      if (*active)
        deviceHash = credentialFingerprint(ua, secretHash);
    }
    ctx.deviceHash = deviceHash;
  }
  else {
    ctx.deviceHash = hashFingerprint(ua, ip);
  }

  req->getAttributes()->insert(AuthContext::kDeviceKey, ctx);
  co_return drogon::HttpResponsePtr{};
}

std::string DeviceFilter::deviceKey(const drogon::HttpRequestPtr& req)
{
  return hashFingerprint(req->getHeader("User-Agent"), resolveIp(req));
}

std::string DeviceFilter::sha256Hex(const std::string& data)
{
  unsigned char digest[SHA256_DIGEST_LENGTH]{};
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest);

  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(sizeof(digest) * 2);
  for (unsigned char byte : digest) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 0x0f]);
  }
  return result;
}

std::string DeviceFilter::credentialFingerprint(const std::string& ua,
                                                const std::string& secretHash)
{
  return hashFingerprint(ua, secretHash);
}

std::string DeviceFilter::hashFingerprint(const std::string& ua,
                                          const std::string& ip)
{
  const std::string finger = ua + "|" + ip;
  const auto key = fingerprintKey();

  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int digestLength = 0;
  if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(finger.data()),
            finger.size(), digest, &digestLength))
    throw std::runtime_error("Could not calculate device fingerprint");

  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(digestLength * 2);
  for (unsigned int i = 0; i < digestLength; ++i) {
    result.push_back(hex[digest[i] >> 4]);
    result.push_back(hex[digest[i] & 0x0f]);
  }
  return result;
}

std::string DeviceFilter::resolveIp(const drogon::HttpRequestPtr& req)
{
  const auto peer = req->getPeerAddr().toIp();
  if (ConfigService::getBool("device.trust_forwarded_for") &&
      trustedProxy(peer)) {
    const auto forwarded = req->getHeader("X-Forwarded-For");
    std::string_view rest(forwarded);
    std::string nearest;
    while (!rest.empty()) {
      const auto comma = rest.rfind(',');
      std::string_view hop =
          comma == std::string_view::npos ? rest : rest.substr(comma + 1);
      rest = comma == std::string_view::npos ? std::string_view{}
                                             : rest.substr(0, comma);
      while (!hop.empty() && hop.front() == ' ')
        hop.remove_prefix(1);
      while (!hop.empty() && hop.back() == ' ')
        hop.remove_suffix(1);
      if (hop.empty())
        continue;
      nearest = std::string(hop);
      if (!trustedProxy(nearest))
        return nearest;
    }
    if (!nearest.empty())
      return nearest;
  }
  return peer;
}

bool DeviceFilter::credentialMode()
{
  return ConfigService::getString("device.identity_mode") == "credential";
}
