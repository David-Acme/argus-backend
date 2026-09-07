#include "device-filter.hxx"

#include <config/app-config.hxx>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <shared/services/config-service/config-service.hxx>
#include <stdexcept>

namespace
{
bool trustedProxy(const std::string& peer)
{
  if (peer == "127.0.0.1" || peer == "::1")
    return true;

  const auto configured = ConfigService::getString("device.trusted_proxy_ips");
  size_t begin = 0;
  while (begin < configured.size()) {
    const auto end = configured.find(',', begin);
    const auto value = configured.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (!value.empty() && value == peer)
      return true;
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return false;
}
} // namespace

drogon::Task<drogon::HttpResponsePtr>
DeviceFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  const auto ua = req->getHeader("User-Agent");
  const auto ip = resolveIp(req);

  DeviceContext ctx;
  ctx.userAgent = ua;
  ctx.ip = ip;
  ctx.deviceHash = hashFingerprint(ua, ip);

  req->getAttributes()->insert(AppConfig::DEVICE_CTX_KEY, ctx);
  co_return drogon::HttpResponsePtr{};
}

std::string DeviceFilter::deviceKey(const drogon::HttpRequestPtr& req)
{
  return hashFingerprint(req->getHeader("User-Agent"), resolveIp(req));
}

std::string DeviceFilter::hashFingerprint(const std::string& ua,
                                          const std::string& ip)
{
  const std::string finger = ua + "|" + ip;
  auto key = ConfigService::getString("device.fingerprint_secret");
  if (key.empty())
    key = ConfigService::getString("jwt.secret");
  if (key.empty())
    throw std::runtime_error("Device fingerprint secret is not configured");

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
    if (!forwarded.empty()) {
      const auto comma = forwarded.find(',');
      return comma != std::string::npos ? forwarded.substr(0, comma)
                                        : forwarded;
    }
  }
  return peer;
}
