#include "service-config.hxx"

#include <link/tls/pinned-key.hxx>

#include <config/config-service.hxx>

namespace
{
uint16_t clampPort(int value, uint16_t fallback)
{
  return value > 0 && value < 65536 ? static_cast<uint16_t>(value)
                                    : fallback;
}

std::chrono::seconds secondsOr(const std::string& key, int fallback)
{
  const int value = ConfigService::getInt(key);
  return std::chrono::seconds{value > 0 ? value : fallback};
}

int positiveOr(const std::string& key, int fallback)
{
  const int value = ConfigService::getInt(key);
  return value > 0 ? value : fallback;
}

tunnel::TunnelMux::Limits resolveLimits()
{
  tunnel::TunnelMux::Limits limits;
  const int idleSeconds = ConfigService::getInt("tunnel.stream_idle_seconds");
  if (idleSeconds > 0)
    limits.idleTimeout = std::chrono::seconds(idleSeconds);
  limits.maxStreams = ConfigService::getInt("tunnel.max_streams");
  if (limits.maxStreams <= 0)
    limits.maxStreams = 256;
  limits.socketSndBuf = ConfigService::getInt("tunnel.socket_snd_buf");
  if (const int perIp = ConfigService::getInt("tunnel.max_streams_per_ip");
      perIp > 0)
    limits.maxStreamsPerIp = perIp;
  return limits;
}

size_t resolvePushQueueCapacity()
{
  const int capacity = ConfigService::getInt("push.queue_capacity");
  return capacity > 0 ? static_cast<size_t>(capacity) : 256;
}

void resolveLink(ClientConfig& config)
{
  config.linkEnabled = ConfigService::getBool("link.enabled");
  config.homeId = ConfigService::getString("link.home_id");

  config.link.host = ConfigService::getString("link.host");
  if (config.link.host.empty())
    config.link.host = "argus-tunnel.lynk.page";
  config.link.port = clampPort(ConfigService::getInt("link.port"), 7001);
  config.link.certificatePath = ConfigService::getString("link.certificate_path");
  if (config.link.certificatePath.empty())
    config.link.certificatePath = "certs/link.crt";
  config.link.keyPath = ConfigService::getString("link.key_path");
  if (config.link.keyPath.empty())
    config.link.keyPath = "certs/link.key";
  const std::string pinnedPath =
      ConfigService::getString("link.node_certificate_path");
  config.link.pinnedSubjectPublicKeyDer =
      tunnel::link::subjectPublicKeyDerOfFile(pinnedPath.empty()
                                                  ? "certs/cloud.crt"
                                                  : pinnedPath)
          .value_or(std::string{});

  config.link.limits.streamPendingCap = static_cast<size_t>(
      positiveOr("link.stream_pending_bytes", 256 * 1024));
  config.link.limits.globalPendingCap = static_cast<size_t>(
      positiveOr("link.global_pending_bytes", 8 * 1024 * 1024));
  config.link.limits.linkHighWater =
      static_cast<size_t>(positiveOr("link.link_high_water_bytes", 256 * 1024));
  config.link.limits.socketSndBuf = ConfigService::getInt("link.socket_snd_buf");
  config.link.limits.maxStreams =
      static_cast<size_t>(positiveOr("link.max_streams", 256));
  config.link.limits.streamIdleTimeout =
      secondsOr("link.stream_idle_seconds", 300);

  config.link.keepaliveActive = secondsOr("link.keepalive_active_seconds", 20);
  config.link.keepaliveIdle = secondsOr("link.keepalive_idle_seconds", 90);
  config.link.keepalivePongTimeout =
      secondsOr("link.keepalive_pong_timeout_seconds", 15);
  config.link.deadLinkTimeout = secondsOr("link.dead_link_seconds", 240);
  config.link.helloTimeout = secondsOr("link.hello_timeout_seconds", 10);
  config.link.reconnectBaseMs = positiveOr("link.reconnect_base_ms", 1000);
  config.link.reconnectCapMs = positiveOr("link.reconnect_cap_ms", 60000);
  config.link.pushQueueCapacity = resolvePushQueueCapacity();
}
}

ClientConfig ClientConfig::resolve()
{
  ClientConfig config;
  config.healthHost = ConfigService::getString("server.health_host");
  if (config.healthHost.empty())
    config.healthHost = "127.0.0.1";
  config.healthPort = clampPort(ConfigService::getInt("server.health_port"),
                                7104);
  config.tunnel.relayHost = ConfigService::getString("server.relay_host");
  if (config.tunnel.relayHost.empty())
    config.tunnel.relayHost = "127.0.0.1";
  config.tunnel.relayPort =
      clampPort(ConfigService::getInt("server.relay_port"), 7101);
  config.tunnel.gatewayHost = ConfigService::getString("server.gateway_host");
  if (config.tunnel.gatewayHost.empty())
    config.tunnel.gatewayHost = "127.0.0.1";
  config.tunnel.gatewayPort =
      clampPort(ConfigService::getInt("server.gateway_port"), 7142);
  config.tunnel.secret = ConfigService::getString("tunnel.secret");
  const int reconnectWaitMs = ConfigService::getInt("tunnel.reconnect_wait_ms");
  config.tunnel.reconnectWaitMs = reconnectWaitMs > 0 ? reconnectWaitMs : 2000;
  const int reconnectMaxWaitMs =
      ConfigService::getInt("tunnel.reconnect_max_wait_ms");
  config.tunnel.reconnectMaxWaitMs =
      reconnectMaxWaitMs > 0 ? reconnectMaxWaitMs : 60000;
  const int pingIntervalSeconds =
      ConfigService::getInt("tunnel.ping_interval_seconds");
  config.tunnel.pingIntervalSeconds = pingIntervalSeconds > 0
                                          ? pingIntervalSeconds
                                          : 30;
  config.tunnel.limits = resolveLimits();
  config.tunnel.pushQueueCapacity = resolvePushQueueCapacity();
  resolveLink(config);
  return config;
}

std::optional<std::string> linkConfigProblem(const ClientConfig& config)
{
  if (!config.linkEnabled)
    return std::nullopt;
  if (config.link.certificatePath.empty())
    return "[link] certificate_path is empty";
  if (config.link.keyPath.empty())
    return "[link] key_path is empty";
  if (config.link.pinnedSubjectPublicKeyDer.empty())
    return "[link] node_certificate_path holds no public key; copy the cloud "
           "node's link.crt there before enabling the link";
  if (config.homeId.size() != 32 ||
      config.homeId.find_first_not_of("0123456789abcdef") != std::string::npos)
    return "[link] home_id must be 32 lowercase hexadecimal characters";
  return std::nullopt;
}

std::optional<std::string> tunnelSecretProblem(std::string_view secret)
{
  if (secret.empty())
    return "[tunnel] secret is empty";
  if (secret.size() < kMinTunnelSecretBytes)
    return "[tunnel] secret is shorter than " +
           std::to_string(kMinTunnelSecretBytes) +
           " bytes; generate one with scripts/provision-host.sh --with-tunnel";
  if (secret.find("CHANGE_ME") != std::string_view::npos)
    return "[tunnel] secret is still the template placeholder";
  return std::nullopt;
}

RelayConfig RelayConfig::resolve()
{
  RelayConfig config;
  config.healthHost = ConfigService::getString("server.health_host");
  if (config.healthHost.empty())
    config.healthHost = "0.0.0.0";
  config.healthPort = clampPort(ConfigService::getInt("server.health_port"),
                                7103);
  config.relay.host = ConfigService::getString("server.host");
  if (config.relay.host.empty())
    config.relay.host = "0.0.0.0";
  config.relay.devicePort =
      clampPort(ConfigService::getInt("server.device_port"), 7100);
  config.relay.homePort =
      clampPort(ConfigService::getInt("server.home_port"), 7101);
  config.relay.secret = ConfigService::getString("tunnel.secret");
  config.relay.limits = resolveLimits();
  config.relay.pushEnabled = ConfigService::getBool("push.enabled");
  config.relay.pushQueueCapacity = resolvePushQueueCapacity();
  return config;
}
