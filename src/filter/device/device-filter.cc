#include "device-filter.hxx"

#include <config/app-config.hxx>
#include <sstream>

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

std::string DeviceFilter::hashFingerprint(const std::string& ua,
                                          const std::string& ip)
{
  const std::string finger = ua + "|" + ip;
  const auto h = std::hash<std::string>{}(finger);
  std::ostringstream oss;
  oss << std::hex << h;
  return oss.str();
}

std::string DeviceFilter::resolveIp(const drogon::HttpRequestPtr& req)
{
  const auto forwarded = req->getHeader("X-Forwarded-For");
  if (!forwarded.empty()) {
    const auto comma = forwarded.find(',');
    return comma != std::string::npos ? forwarded.substr(0, comma) : forwarded;
  }
  const auto addr = req->getPeerAddr();
  return addr.toIp();
}
