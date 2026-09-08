#pragma once

#include <client/tunnel-client.hxx>
#include <relay/tunnel-relay.hxx>

#include <json/value.h>
#include <string>

struct ClientConfig
{
  std::string healthHost{"127.0.0.1"};
  uint16_t healthPort{7104};
  tunnel::ClientOptions tunnel;

  // Resolves [tunnel] keys plus the client's [server] keys (relay target, gateway remote listener, health listener).
  static ClientConfig resolve();
};

struct RelayConfig
{
  std::string healthHost{"0.0.0.0"};
  uint16_t healthPort{7103};
  tunnel::RelayOptions relay;

  // Resolves the [tunnel] behavior keys plus the relay's [server] keys (bind host, device/home/health listeners).
  static RelayConfig resolve();
};

// The /health listener JSON for either binary (plain HTTP, frozen envelope).
Json::Value healthListenerJson(const std::string& host, uint16_t port);
uint16_t resolvePort(const char* key, uint16_t fallback);
