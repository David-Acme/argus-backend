#pragma once

#include <drogon/HttpRequest.h>
#include <cstdint>

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
