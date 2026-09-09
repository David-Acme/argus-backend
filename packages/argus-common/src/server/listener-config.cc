#include "listener-config.hxx"

#include <shared/services/config-service/config-service.hxx>

namespace
{

std::string configString(const char* key, const char* fallback)
{
  auto value = ConfigService::getString(key);
  return value.empty() ? fallback : value;
}

uint16_t resolvePort(const char* key, uint16_t fallback)
{
  const int port = ConfigService::getInt(key);
  return port > 0 ? static_cast<uint16_t>(port) : fallback;
}

} // namespace

ListenerConfig ListenerConfig::resolve(uint16_t defaultPort,
                                       const char* portKey)
{
  ListenerConfig config;
  config.host = configString("server.host", "127.0.0.1");
  config.port = resolvePort(portKey, defaultPort);
  return config;
}

ListenerConfig ListenerConfig::resolveTls(uint16_t defaultPort)
{
  ListenerConfig config;
  config.host = configString("gateway.host", "0.0.0.0");
  config.port = resolvePort("gateway.port", defaultPort);
  // Plain-HTTP is a local-test option ([gateway] plain = true); the cutover
  // listener is TLS by default.
  config.tls = !ConfigService::getBool("gateway.plain");
  config.certPath = configString("cert.server_cert", "certs/server.pem");
  config.keyPath = configString("cert.server_key", "certs/server.key");
  config.minTlsProtocol = configString("gateway.min_protocol", "TLSv1.2");
  return config;
}

GrpcListenerConfig GrpcListenerConfig::resolve(uint16_t defaultPort,
                                               const char* portKey)
{
  GrpcListenerConfig config;
  config.host = configString("server.host", "127.0.0.1");
  config.port = resolvePort(portKey, defaultPort);
  return config;
}

Json::Value singleListenerJson(const ListenerConfig& base, int port)
{
  Json::Value listener(Json::objectValue);
  listener["address"] = base.host;
  listener["port"] = port;
  listener["https"] = base.tls;
  if (base.tls) {
    listener["cert"] = base.certPath;
    listener["key"] = base.keyPath;
    Json::Value sslConf(Json::arrayValue);
    Json::Value protocol(Json::arrayValue);
    protocol.append("MinProtocol");
    protocol.append(base.minTlsProtocol);
    sslConf.append(protocol);
    listener["ssl_conf"] = sslConf;
  }
  return listener;
}

Json::Value listenerJson(const ListenerConfig& config)
{
  Json::Value listeners(Json::arrayValue);
  listeners.append(singleListenerJson(config, config.port));
  return listeners;
}
