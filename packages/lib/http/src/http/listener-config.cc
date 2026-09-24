#include "listener-config.hxx"

#include <config/config-service.hxx>

namespace
{

constexpr std::string_view kDefaultHost = "127.0.0.1";
constexpr std::string_view kDefaultServiceHost = "0.0.0.0";
constexpr std::string_view kDefaultCertPath = "certs/server.pem";
constexpr std::string_view kDefaultKeyPath = "certs/server.key";
constexpr std::string_view kDefaultMinTlsProtocol = "TLSv1.2";

std::string configString(const std::string& key, std::string_view fallback)
{
  auto value = ConfigService::getString(key);
  return value.empty() ? std::string(fallback) : value;
}

uint16_t resolvePort(const std::string& key, uint16_t fallback)
{
  const int port = ConfigService::getInt(key);
  return port > 0 ? static_cast<uint16_t>(port) : fallback;
}

}

ListenerConfig ListenerConfig::resolve(uint16_t defaultPort,
                                       std::string_view portKey)
{
  ListenerConfig config;
  config.host = configString("server.host", kDefaultHost);
  config.port = resolvePort(std::string(portKey), defaultPort);
  return config;
}

ListenerConfig ListenerConfig::resolveServiceTls(std::string_view section,
                                                 uint16_t defaultPort)
{
  const std::string prefix = std::string(section) + '.';
  ListenerConfig config;
  config.host = configString(prefix + "host", kDefaultServiceHost);
  config.port = resolvePort(prefix + "port", defaultPort);
  config.tls = !ConfigService::getBool(prefix + "plain");
  config.certPath = configString("cert.server_cert", kDefaultCertPath);
  config.keyPath = configString("cert.server_key", kDefaultKeyPath);
  config.minTlsProtocol =
      configString(prefix + "min_protocol", kDefaultMinTlsProtocol);
  return config;
}

GrpcListenerConfig GrpcListenerConfig::resolve(uint16_t defaultPort,
                                               std::string_view portKey)
{
  GrpcListenerConfig config;
  config.host = configString("server.host", kDefaultHost);
  config.port = resolvePort(std::string(portKey), defaultPort);
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
