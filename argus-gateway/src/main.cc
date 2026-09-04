#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <memory>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>

#include <cstdint>
#include <string>

int main()
{
  ConfigService::load("config.toml");

  drogon::app().registerController(std::make_shared<HealthController>());

  const std::string natsUrl = ConfigService::getString("nats.url");
  std::unique_ptr<NatsBus> natsBus;
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; event bus disabled";
  } else {
    natsBus = std::make_unique<NatsBus>();
    if (natsBus->connect())
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; continuing without it";
  }

  const int port = ConfigService::getInt("gateway.port");
  std::string host = ConfigService::getString("gateway.host");
  if (host.empty())
    host = "0.0.0.0";

  drogon::app()
      .addListener(host, static_cast<uint16_t>(port > 0 ? port : 7024))
      .setThreadNum(0)
      .run();
  return 0;
}
