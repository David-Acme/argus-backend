#pragma once

#include <json/value.h>
#include <string>

struct ListenerConfig
{
  std::string host;
  uint16_t port{0};
  bool tls{false};
  std::string certPath;
  std::string keyPath;
  std::string minTlsProtocol;

  // Resolves [gateway] host/port/tls/min_protocol plus the [cert] server
  // key pair, mirroring the legacy listener shape (same keys, same certs
  // directory) so the app's pinned CA stays byte-identical.
  static ListenerConfig resolve();
};

// The [[listeners]] JSON array Drogon consumes from the loaded config.
Json::Value listenerJson(const ListenerConfig& config);
