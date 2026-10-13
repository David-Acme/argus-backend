#include "remote-config.hxx"

#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <http/certificate-reload.hxx>
#include <http/listener-config.hxx>
#include <stdexcept>
#include <string>

RemoteConfig RemoteConfig::resolve()
{
  RemoteConfig config;
  const int port = ConfigService::getInt("remote.tunnel_port");
  if (port > 0 && port < 65536)
    config.tunnelPort = static_cast<uint16_t>(port);
  config.enabled = ConfigService::getBool("remote.enabled");
  config.allowQrLogin = ConfigService::getBool("remote.allow_qr_login");
  config.tunnelProfile = ConfigService::getBool("remote.tunnel_profile");
  config.tunnelCert = ConfigService::getString("remote.tunnel_cert");
  config.tunnelKey = ConfigService::getString("remote.tunnel_key");
  return config;
}

ListenerConfig remoteListenerConfig(const RemoteConfig& remote,
                                    const ListenerConfig& base)
{
  ListenerConfig listener = base;
  listener.port = remote.tunnelPort;
  if (base.tls &&
      certificate_reload::usablePair(
          {.certPath = remote.tunnelCert, .keyPath = remote.tunnelKey})) {
    listener.certPath = remote.tunnelCert;
    listener.keyPath = remote.tunnelKey;
  }
  return listener;
}

void watchRemoteCertificate(const RemoteConfig& remote,
                            const ListenerConfig& base)
{
  if (remote.tunnelPort == 0 || remote.tunnelCert.empty())
    return;
  const ListenerConfig tunnel = remoteListenerConfig(remote, base);
  if (tunnel.certPath == remote.tunnelCert)
    certificate_reload::watch(tunnel);
}

bool requestIsRemote(const drogon::HttpRequestPtr& req,
                     const RemoteConfig& config)
{
  return config.tunnelPort != 0
         && req->localAddr().toPort() == config.tunnelPort;
}

void appendRemoteListener(const AppendRemoteListenerInput& input)
{
  const RemoteConfig& remote = input.remote;
  if (remote.tunnelPort == 0)
    return;
  const ListenerConfig tunnel = remoteListenerConfig(remote, input.base);
  const bool pairConfigured = !remote.tunnelCert.empty() || !remote.tunnelKey.empty();
  if (pairConfigured && input.base.tls && tunnel.certPath != remote.tunnelCert)
    LOG_WARN << "[remote] tunnel_cert and tunnel_key are not a usable pair, so the "
                "tunnel listener on port "
             << remote.tunnelPort << " presents the service certificate";
  input.listeners.append(
      singleListenerJson(tunnel, static_cast<int>(remote.tunnelPort)));
}

void requireDistinctTunnelPort(const ListenerConfig& listener,
                               const RemoteConfig& remote)
{
  if (remote.tunnelPort != 0 && remote.tunnelPort == listener.port)
    throw std::runtime_error("[remote] tunnel_port "
                             + std::to_string(remote.tunnelPort)
                             + " collides with the service listener port "
                             + std::to_string(listener.port));
}

void requireTunnelListener(const RemoteConfig& remote)
{
  if (remote.tunnelPort != 0)
    return;
  if (remote.tunnelProfile || remote.enabled || remote.allowQrLogin)
    throw std::runtime_error(
        "[remote] tunnel_port is 0 while the tunnel is configured "
        "(tunnel_profile, enabled or allow_qr_login): every tunnelled request "
        "would be taken for a LAN one");
}
