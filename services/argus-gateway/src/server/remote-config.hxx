#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>
#include <json/value.h>
#include <server/listener-config.hxx>

struct RemoteConfig
{
  // 0 keeps the single-listener shape (remote access disabled).
  uint16_t tunnelPort{0};
  bool enabled{false};

  // Ruling CG: resolves [remote] tunnel_port/enabled.
  static RemoteConfig resolve();
};

// Ruling CG: the local port the connection landed on is the only honest signal.
bool requestIsRemote(const drogon::HttpRequestPtr& req,
                     const RemoteConfig& config);

// Appends the tunnel listener with the public listener's TLS posture.
void appendRemoteListener(Json::Value& listeners, const RemoteConfig& remote,
                          const ListenerConfig& base);

// Fails fast when the tunnel listener would collide with the public one.
void requireDistinctTunnelPort(const ListenerConfig& listener,
                               const RemoteConfig& remote);
