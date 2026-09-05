#pragma once

#include <json/value.h>
#include <string>

struct ListenerConfig
{
  std::string host;
  uint16_t port{0};

  // Resolves the [server] internal plain-HTTP listener (loopback by default:
  // the gateway is the only public listener once the F2-2 cutover routes
  // /camera* and /zone here).
  static ListenerConfig resolve();
};

// The [[listeners]] JSON array Drogon consumes from the loaded config.
Json::Value listenerJson(const ListenerConfig& config);
