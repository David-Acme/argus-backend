#pragma once

#include <string>
#include <vector>

struct ProxyConfig
{
  // Base URL of argus-camera: the /camera and /zone routes (CRUD, zone CRUD
  // and the device-control paths) route here, every segment depth up to the
  // route cap. Empty leaves the camera domain unrouted.
  std::string cameraProxyUrl;
  // Base URL of argus-productivity: the calendar-event, calendar-event-share,
  // project, project-member and project-task routes route here entirely
  // (Ruling AP). Empty leaves the domain unrouted.
  std::string productivityProxyUrl;
  // Base URL of argus-notification: the /notification and /notification-token
  // routes route here (Ruling AP). Empty leaves the domain unrouted.
  std::string notificationProxyUrl;
  // Path prefixes served natively by the gateway: never forwarded.
  std::vector<std::string> exclusions;

  static ProxyConfig resolve();
};

// Every path the gateway serves itself (identity surface, /sync socket,
// health). The proxy forwards everything else, so each prefix here must cover
// at least one registered route.
const std::vector<std::string>& gatewayNativePaths();

// Segment-boundary prefix match: the prefix alone or followed by "/".
bool isGatewayNativePath(const std::string& path,
                         const std::vector<std::string>& exclusions);
