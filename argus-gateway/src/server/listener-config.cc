#include "listener-config.hxx"

#include <shared/services/config-service/config-service.hxx>

ListenerConfig ListenerConfig::resolve()
{
  ListenerConfig config;
  config.host = ConfigService::getString("gateway.host");
  if (config.host.empty())
    config.host = "0.0.0.0";
  config.port = static_cast<uint16_t>(
      ConfigService::getInt("gateway.port") > 0
          ? ConfigService::getInt("gateway.port")
          : 7024);
  // Plain-HTTP is a local-test option ([gateway] plain = true); the cutover
  // listener is TLS by default.
  config.tls = !ConfigService::getBool("gateway.plain");
  config.certPath = ConfigService::getString("cert.server_cert");
  if (config.certPath.empty())
    config.certPath = "certs/server.pem";
  config.keyPath = ConfigService::getString("cert.server_key");
  if (config.keyPath.empty())
    config.keyPath = "certs/server.key";
  config.minTlsProtocol = ConfigService::getString("gateway.min_protocol");
  if (config.minTlsProtocol.empty())
    config.minTlsProtocol = "TLSv1.2";
  return config;
}

Json::Value listenerJson(const ListenerConfig& config)
{
  Json::Value listeners(Json::arrayValue);
  Json::Value listener(Json::objectValue);
  listener["address"] = config.host;
  listener["port"] = Json::Value::Int(config.port);
  listener["https"] = config.tls;
  if (config.tls) {
    listener["cert"] = config.certPath;
    listener["key"] = config.keyPath;
    Json::Value sslConf(Json::arrayValue);
    Json::Value protocol(Json::arrayValue);
    protocol.append("MinProtocol");
    protocol.append(config.minTlsProtocol);
    sslConf.append(protocol);
    listener["ssl_conf"] = sslConf;
  }
  listeners.append(listener);
  return listeners;
}
