#pragma once

#include <string>
#include <vector>

struct ProxyConfig
{
  std::string authProxyUrl;
  std::string cameraProxyUrl;
  std::string productivityProxyUrl;
  std::string notificationProxyUrl;
  std::string guardProxyUrl;
  std::vector<std::string> exclusions;

  static ProxyConfig resolve();
};

const std::vector<std::string>& gatewayNativePaths();

bool isGatewayNativePath(const std::string& path,
                         const std::vector<std::string>& exclusions);
