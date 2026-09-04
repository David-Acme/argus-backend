#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <controllers/health-controller.hxx>
#include <identity/identity-config.hxx>
#include <identity/identity-registrar.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

#include <json/json.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace
{

Json::Value parseBody(const drogon::HttpResponsePtr& response)
{
  Json::Value body;
  Json::Reader reader;
  const std::string text(response->getBody().begin(),
                         response->getBody().end());
  CHECK(reader.parse(text, body));
  return body;
}

} // namespace

TEST_CASE("health envelope conforms to the ApiResponse shape")
{
  auto response = ApiResponse::ok(HealthController::info(12.5));

  const Json::Value body = parseBody(response);

  CHECK(body["status"].isInt());
  CHECK(body["status"].asInt() == 200);
  CHECK(body["info"]["service"] == "argus-gateway");
  CHECK(body["info"]["uptimeSeconds"].asDouble() == doctest::Approx(12.5));
  CHECK(body.isMember("errors"));
  CHECK(response->getStatusCode() == drogon::k200OK);
  CHECK(response->getContentType() == drogon::CT_APPLICATION_JSON);
}

TEST_CASE("identity config resolves database and schema with defaults")
{
  const char* path = "gateway-test-config-empty.toml";
  {
    std::ofstream file(path);
    file << "[gateway]\n"
         << "port = 7024\n";
  }

  ConfigService::load(path);
  const IdentityDbConfig config = IdentityConfig::resolveDb();

  CHECK(config.dbPath == "database/identity.db");
  CHECK(config.schemaPath == "database/identity-schema.sql");

  std::remove(path);
}

TEST_CASE("identity config honors the identity section overrides")
{
  const char* path = "gateway-test-config-identity.toml";
  {
    std::ofstream file(path);
    file << "[identity]\n"
         << "db = \"/tmp/argus-test/identity.db\"\n"
         << "schema = \"/tmp/argus-test/identity-schema.sql\"\n";
  }

  ConfigService::load(path);
  const IdentityDbConfig config = IdentityConfig::resolveDb();

  CHECK(config.dbPath == "/tmp/argus-test/identity.db");
  CHECK(config.schemaPath == "/tmp/argus-test/identity-schema.sql");

  std::remove(path);
}

TEST_CASE("runtime override wins over the config file value")
{
  const char* path = "gateway-test-config-runtime.toml";
  {
    std::ofstream file(path);
    file << "[database]\n"
         << "file = \"database/argus.db\"\n";
  }

  ConfigService::load(path);
  CHECK(ConfigService::getString("database.file") == "database/argus.db");

  ConfigService::setRuntimeString("database.file", "database/identity.db");
  CHECK(ConfigService::getString("database.file") == "database/identity.db");

  std::remove(path);
}

TEST_CASE("identity surface registers controllers and filters once")
{
  const char* path = "gateway-test-config-register.toml";
  {
    std::ofstream file(path);
    file << "[jwt]\n"
         << "secret = \"0123456789abcdef0123456789abcdef0123456789\"\n"
         << "refresh_secret = \"fedcba9876543210fedcba9876543210fedcba98\"\n"
         << "access_ttl_minutes = 15\n"
         << "refresh_ttl_days = 30\n";
  }

  ConfigService::load(path);
  std::remove(path);

  const IdentityRegistrationStats stats = registerIdentitySurface();

  CHECK(stats.controllers == 5);
  CHECK(stats.filters == 4);
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
