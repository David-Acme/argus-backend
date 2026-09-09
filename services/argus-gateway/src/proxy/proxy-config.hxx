#pragma once

#include <string>
#include <vector>

struct ProxyConfig
{
  // Base URL of argus-camera; empty leaves the camera domain unrouted.
  std::string cameraProxyUrl;
  // Base URL of argus-productivity (Ruling AP); empty leaves it unrouted.
  std::string productivityProxyUrl;
  // Base URL of argus-notification (Ruling AP); empty leaves it unrouted.
  std::string notificationProxyUrl;
  // Path prefixes served natively by the gateway: never forwarded.
  std::vector<std::string> exclusions;

  static ProxyConfig resolve();
};

// Every path the gateway serves itself; the proxy forwards everything else.
const std::vector<std::string>& gatewayNativePaths();

// Segment-boundary prefix match: the prefix alone or followed by "/".
bool isGatewayNativePath(const std::string& path,
                         const std::vector<std::string>& exclusions);
