#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>
#include <http/listener-config.hxx>
#include <json/value.h>
#include <string>

struct RemoteConfig
{
  uint16_t tunnelPort{0};
  bool enabled{false};
  bool allowQrLogin{false};
  bool tunnelProfile{false};
  std::string tunnelCert;
  std::string tunnelKey;

  static RemoteConfig resolve();
};

ListenerConfig remoteListenerConfig(const RemoteConfig& remote,
                                    const ListenerConfig& base);

void watchRemoteCertificate(const RemoteConfig& remote,
                            const ListenerConfig& base);

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

void requireTunnelListener(const RemoteConfig& remote);
