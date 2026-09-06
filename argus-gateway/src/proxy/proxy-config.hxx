#pragma once

#include <string>
#include <vector>

struct ProxyConfig
{
  // Base URL of the legacy backend's internal listener; empty disables the
  // proxy (the gateway then serves nothing beyond its own routes).
  std::string upstreamUrl;
  // Base URL of argus-camera: the /camera and /zone CRUD routes (two path
  // segments) route here while the deeper control paths keep falling through
  // to the legacy. Empty keeps the camera routes on the legacy.
  std::string cameraProxyUrl;
  // Base URL of argus-productivity: the calendar-event, calendar-event-share,
  // project, project-member and project-task routes route here entirely
  // (Ruling AP). Empty keeps them on the legacy.
  std::string productivityProxyUrl;
  // Base URL of argus-notification: PATCH /notification/read and POST
  // /notification-token route here (Ruling AP). Empty keeps them on the
  // legacy.
  std::string notificationProxyUrl;
  // Path prefixes served natively by the gateway: never forwarded.
  std::vector<std::string> exclusions;

  static ProxyConfig resolve();
};

// Every path the gateway serves itself (identity surface, /sync socket,
// health). The proxy forwards everything else, so each prefix here must cover
// at least one registered route and no legacy-owned route.
const std::vector<std::string>& gatewayNativePaths();

// Segment-boundary prefix match: the prefix alone or followed by "/".
bool isGatewayNativePath(const std::string& path,
                         const std::vector<std::string>& exclusions);
