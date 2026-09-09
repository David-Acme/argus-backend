#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <camera/camera-config.hxx>
#include <controllers/health-controller.hxx>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

#include <cstdio>
#include <fstream>
#include <string>

TEST_CASE("camera config resolves database and schema with defaults")
{
  const char* path = "camera-config-test-empty.toml";
  {
    std::ofstream file(path);
    file << "[server]\n"
         << "port = 7026\n";
  }

  ConfigService::load(path);
  const CameraDbConfig config = CameraConfig::resolveDb();
  const ListenerConfig listener = ListenerConfig::resolve(7026);

  CHECK(config.dbPath == "database/camera.db");
  CHECK(config.schemaPath == "services/argus-camera/database/schema.sql");
  CHECK(listener.host == "127.0.0.1");
  CHECK(listener.port == 7026);

  std::remove(path);
}

TEST_CASE("camera config honors the camera and server overrides")
{
  const char* path = "camera-config-test-overrides.toml";
  {
    std::ofstream file(path);
    file << "[camera]\n"
         << "db = \"/tmp/argus-test/camera.db\"\n"
         << "schema = \"/tmp/argus-test/camera-schema.sql\"\n"
         << "[server]\n"
         << "host = \"0.0.0.0\"\n"
         << "port = 7027\n";
  }

  ConfigService::load(path);
  const CameraDbConfig config = CameraConfig::resolveDb();
  const ListenerConfig listener = ListenerConfig::resolve(7026);

  CHECK(config.dbPath == "/tmp/argus-test/camera.db");
  CHECK(config.schemaPath == "/tmp/argus-test/camera-schema.sql");
  CHECK(listener.host == "0.0.0.0");
  CHECK(listener.port == 7027);

  std::remove(path);
}

TEST_CASE("camera listener JSON is a plain internal listener")
{
  ListenerConfig config;
  config.host = "127.0.0.1";
  config.port = 7026;

  const Json::Value listeners = listenerJson(config);

  REQUIRE(listeners.size() == 1);
  CHECK(listeners[0]["address"] == "127.0.0.1");
  CHECK(listeners[0]["port"].asInt() == 7026);
  CHECK(listeners[0]["https"].asBool() == false);
  CHECK_FALSE(listeners[0].isMember("cert"));
}

TEST_CASE("camera health envelope carries the camera service name")
{
  auto response = ApiResponse::ok(HealthController::info("argus-camera", 3.5));

  const Json::Value body = [&response] {
    Json::Value parsed;
    Json::Reader reader;
    const std::string text(response->getBody().begin(),
                           response->getBody().end());
    CHECK(reader.parse(text, parsed));
    return parsed;
  }();

  CHECK(body["status"].asInt() == 200);
  CHECK(body["info"]["service"] == "argus-camera");
  CHECK(body["info"]["uptimeSeconds"].asDouble() == doctest::Approx(3.5));
  CHECK(body.isMember("errors"));
  CHECK(response->getStatusCode() == drogon::k200OK);
}
