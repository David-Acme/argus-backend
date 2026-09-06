#include "listener-config.hxx"

#include <shared/services/config-service/config-service.hxx>

ListenerConfig ListenerConfig::resolve()
{
  ListenerConfig config;
  config.host = ConfigService::getString("server.host");
  if (config.host.empty())
    config.host = "127.0.0.1";
  const int port = ConfigService::getInt("server.port");
  config.port = port > 0 ? static_cast<uint16_t>(port) : 7030;
  return config;
}

Json::Value listenerJson(const ListenerConfig& config)
{
  Json::Value listeners(Json::arrayValue);
  Json::Value listener(Json::objectValue);
  listener["address"] = config.host;
  listener["port"] = Json::Value::Int(config.port);
  listener["https"] = false;
  listeners.append(listener);
  return listeners;
}
