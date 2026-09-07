#pragma once

#include <json/value.h>
#include <server/remote-config.hxx>
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

// Ruling CG: appends the tunnel listener with the public listener's TLS
// posture (same host, certs and min protocol); no-op when tunnel_port is 0.
void appendRemoteListener(Json::Value& listeners, const RemoteConfig& remote,
                          const ListenerConfig& base);
