#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

// The HTTP listener every service builds its Drogon config from. Services
// bind one plain loopback listener; the gateway binds the public TLS one.
struct ListenerConfig
{
  std::string host;
  uint16_t port{0};
  bool tls{false};
  std::string certPath;
  std::string keyPath;
  std::string minTlsProtocol;

  // Plain internal listener: [server] host (default 127.0.0.1) and portKey
  // (default [server] port), falling back to the service's own default.
  static ListenerConfig resolve(uint16_t defaultPort,
                                const char* portKey = "server.port");

  // The gateway's public listener: [gateway] host/port/plain/min_protocol
  // plus the [cert] server key pair, mirroring the legacy listener shape so
  // the app's pinned CA stays byte-identical.
  static ListenerConfig resolveTls(uint16_t defaultPort);
};

// The internal gRPC listener, for services that serve one.
struct GrpcListenerConfig
{
  std::string host;
  uint16_t port{0};

  static GrpcListenerConfig resolve(uint16_t defaultPort,
                                    const char* portKey = "server.grpc_port");
};

// The [[listeners]] JSON array Drogon consumes from the loaded config.
Json::Value listenerJson(const ListenerConfig& config);

// One listener entry, for callers that assemble several (the gateway's
// tunnel listener rides the public listener's TLS posture).
Json::Value singleListenerJson(const ListenerConfig& base, int port);
