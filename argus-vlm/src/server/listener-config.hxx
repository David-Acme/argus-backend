#pragma once

#include <json/value.h>
#include <string>

struct ListenerConfig
{
  std::string host;
  uint16_t port{0};

  // Resolves the [server] internal plain-HTTP listener (loopback by default:
  // the wire is internal-network only and never exposed through the gateway).
  static ListenerConfig resolve();
};

// The [[listeners]] JSON array Drogon consumes from the loaded config.
Json::Value listenerJson(const ListenerConfig& config);