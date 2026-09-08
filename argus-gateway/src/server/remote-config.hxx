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

// Ruling CG: TLS is end-to-end through the tunnel relay, so the local port
// the connection landed on is the only honest remote signal.
bool requestIsRemote(const drogon::HttpRequestPtr& req,
                     const RemoteConfig& config);

// Ruling CG: appends the tunnel listener with the public listener's TLS
// posture (same host, certs and min protocol); no-op when tunnel_port is 0.
void appendRemoteListener(Json::Value& listeners, const RemoteConfig& remote,
                          const ListenerConfig& base);

// Fails fast when the tunnel listener would collide with the public one;
// otherwise the duplicate bind aborts startup without a config message.
void requireDistinctTunnelPort(const ListenerConfig& listener,
                               const RemoteConfig& remote);
