#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>

// The HTTP listener every service builds its Drogon config from.
struct ListenerConfig
{
  std::string host;
  uint16_t port{0};
  bool tls{false};
  std::string certPath;
  std::string keyPath;
  std::string minTlsProtocol;

  // Plain internal listener: [server] host and portKey, service default as fallback.
  static ListenerConfig resolve(uint16_t defaultPort,
                                const char* portKey = "server.port");

  // The gateway's public listener, mirroring the legacy shape.
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

// One listener entry, for callers that assemble several.
Json::Value singleListenerJson(const ListenerConfig& base, int port);
