#pragma once

#include <json/value.h>
#include <string>

struct ListenerConfig
{
  std::string host;
  uint16_t port{0};

  // Resolves the /health HTTP listener ([server].host + health_port).
  static ListenerConfig resolve();
};

struct GrpcListenerConfig
{
  std::string host;
  uint16_t port{0};

  // Resolves the VoiceService gRPC listener ([server].host + grpc_port).
  static GrpcListenerConfig resolve();
};

// The [[listeners]] JSON array Drogon consumes from the loaded config.
Json::Value listenerJson(const ListenerConfig& config);
