#include "service-config.hxx"

#include <shared/services/config-service/config-service.hxx>

namespace
{
uint16_t clampPort(int value, uint16_t fallback)
{
  return value > 0 && value < 65536 ? static_cast<uint16_t>(value)
                                    : fallback;
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
  return limits;
}

size_t resolvePushQueueCapacity()
{
  const int capacity = ConfigService::getInt("push.queue_capacity");
  return capacity > 0 ? static_cast<size_t>(capacity) : 256;
}
} // namespace

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
      clampPort(ConfigService::getInt("server.gateway_port"), 7024);
  config.tunnel.secret = ConfigService::getString("tunnel.secret");
  const int reconnectWaitMs = ConfigService::getInt("tunnel.reconnect_wait_ms");
  config.tunnel.reconnectWaitMs = reconnectWaitMs > 0 ? reconnectWaitMs : 2000;
  const int maxReconnects = ConfigService::getInt("tunnel.max_reconnects");
  config.tunnel.maxReconnects = maxReconnects >= 0 ? maxReconnects : 60;
  const int pingIntervalSeconds =
      ConfigService::getInt("tunnel.ping_interval_seconds");
  config.tunnel.pingIntervalSeconds = pingIntervalSeconds > 0
                                          ? pingIntervalSeconds
                                          : 30;
  config.tunnel.limits = resolveLimits();
  config.tunnel.pushQueueCapacity = resolvePushQueueCapacity();
  return config;
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

uint16_t resolvePort(const char* key, uint16_t fallback)
{
  return clampPort(ConfigService::getInt(key), fallback);
}

Json::Value healthListenerJson(const std::string& host, uint16_t port)
{
  Json::Value listeners(Json::arrayValue);
  Json::Value listener(Json::objectValue);
  listener["address"] = host;
  listener["port"] = Json::Value::Int(port);
  listener["https"] = false;
  listeners.append(listener);
  return listeners;
}
