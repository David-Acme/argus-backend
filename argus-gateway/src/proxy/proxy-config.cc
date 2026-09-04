#include "proxy-config.hxx"

#include <shared/services/config-service/config-service.hxx>

const std::vector<std::string>& gatewayNativePaths()
{
  static const std::vector<std::string> paths = {
      "/auth",
      "/invitation",
      "/pairing",
      "/portrait-preview",
      "/user",
      "/sync",
      "/health",
  };
  return paths;
}

bool isGatewayNativePath(const std::string& path,
                         const std::vector<std::string>& exclusions)
{
  for (const auto& prefix : exclusions) {
    if (path == prefix)
      return true;
    if (path.rfind(prefix + "/", 0) == 0)
      return true;
  }
  return false;
}

ProxyConfig ProxyConfig::resolve()
{
  ProxyConfig config;
  config.upstreamUrl = ConfigService::getString("legacy.proxy_url");
  config.exclusions = gatewayNativePaths();
  return config;
}