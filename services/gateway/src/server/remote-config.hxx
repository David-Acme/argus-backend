#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>
#include <http/listener-config.hxx>
#include <json/value.h>

struct RemoteConfig
{
  uint16_t tunnelPort{0};
  bool enabled{false};

  static RemoteConfig resolve();
};

bool requestIsRemote(const drogon::HttpRequestPtr& req,
                     const RemoteConfig& config);

struct AppendRemoteListenerInput
{
  Json::Value& listeners;
  const RemoteConfig& remote;
  const ListenerConfig& base;
};

void appendRemoteListener(const AppendRemoteListenerInput& input);

void requireDistinctTunnelPort(const ListenerConfig& listener,
                               const RemoteConfig& remote);
