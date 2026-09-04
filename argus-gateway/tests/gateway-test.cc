#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <controllers/health-controller.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <json/json.h>

#include <cstdio>
#include <fstream>

TEST_CASE("health envelope carries ok status and service identity")
{
  const Json::Value body = HealthController::envelope(12.5);

  CHECK(body["status"] == "ok");
  CHECK(body["info"]["service"] == "argus-gateway");
  CHECK(body["info"]["uptimeSeconds"].asDouble() == doctest::Approx(12.5));
}

TEST_CASE("gateway config section resolves listener and nats keys")
{
  const char* path = "gateway-test-config.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n"
         << "host = \"127.0.0.1\"\n"
         << "\n"
         << "[nats]\n"
         << "url = \"\"\n";
  }

  ConfigService::load(path);

  CHECK(ConfigService::getInt("gateway.port") == 7024);
  CHECK(ConfigService::getString("gateway.host") == "127.0.0.1");
  CHECK(ConfigService::getString("nats.url").empty());

  std::remove(path);
}
