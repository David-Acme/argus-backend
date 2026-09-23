#pragma once

#include <cstdint>
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

  static ListenerConfig resolve(uint16_t defaultPort,
                                const char* portKey = "server.port");

  static ListenerConfig resolveTls(uint16_t defaultPort);
};

struct GrpcListenerConfig
{
  std::string host;
  uint16_t port{0};

  static GrpcListenerConfig resolve(uint16_t defaultPort,
                                    const char* portKey = "server.grpc_port");
};

Json::Value listenerJson(const ListenerConfig& config);

Json::Value singleListenerJson(const ListenerConfig& base, int port);
