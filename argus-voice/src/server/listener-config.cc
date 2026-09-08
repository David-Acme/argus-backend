#include "listener-config.hxx"

#include <shared/services/config-service/config-service.hxx>

namespace
{
std::string resolveHost()
{
  std::string host = ConfigService::getString("server.host");
  return host.empty() ? "127.0.0.1" : host;
}

uint16_t resolvePort(const char* key, uint16_t fallback)
{
  const int port = ConfigService::getInt(key);
  return port > 0 ? static_cast<uint16_t>(port) : fallback;
}
} // namespace

ListenerConfig ListenerConfig::resolve()
{
  ListenerConfig config;
  config.host = resolveHost();
  config.port = resolvePort("server.health_port", 7035);
  return config;
}

GrpcListenerConfig GrpcListenerConfig::resolve()
{
  GrpcListenerConfig config;
  config.host = resolveHost();
  config.port = resolvePort("server.grpc_port", 7034);
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
