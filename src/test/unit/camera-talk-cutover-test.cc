#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "fake-tts-server.hxx"

#include <config/app-config.hxx>
#include <feature/api/camera-control/controllers/camera-control-controller.hxx>
#include <feature/api/camera-control/services/camera-control-feature-service.hxx>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <json/value.h>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kCameraDb = "camera-talk-cutover-test.db";

struct StubSpeaker final : ICameraDriver
{
  int speakCalls{0};
  size_t lastSamples{0};
  int lastRate{0};

  Json::Value capabilities() const override { return Json::Value(); }
  DriverResult status() override { return DriverResult::failure("noop"); }
  DriverResult presets() override { return DriverResult::failure("noop"); }
  DriverResult move(const DriverMoveInput&) override
  {
    return DriverResult::failure("noop");
  }
  DriverResult preset(const DriverPresetInput&) override
  {
    return DriverResult::failure("noop");
  }
  DriverResult settings(const DriverSettingsInput&) override
  {
    return DriverResult::failure("noop");
  }
  DriverResult speak(const DriverSpeakInput& input) override
  {
    ++speakCalls;
    lastSamples = input.samples.size();
    lastRate = input.sampleRate;
    return DriverResult{.ok = true, .error = "", .data = Json::Value()};
  }
};

// A bound-then-closed port: every connect is refused on the loopback.
int deadPort()
{
  int probe = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &len);
  const int port = ntohs(addr.sin_port);
  ::close(probe);
  return port;
}

void seedCameraDb()
{
  std::remove(kCameraDb);
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") +
                                                  kCameraDb,
                                              1);
  client->execSqlSync(
      "CREATE TABLE camera ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "name TEXT NOT NULL, manufacturer TEXT NOT NULL DEFAULT '', "
      "model TEXT NOT NULL DEFAULT '', ip TEXT NOT NULL, "
      "port INTEGER NOT NULL DEFAULT 554, "
      "username TEXT NOT NULL DEFAULT 'admin', "
      "password TEXT NOT NULL DEFAULT '', "
      "cloud_username TEXT NOT NULL DEFAULT '', "
      "cloud_password TEXT NOT NULL DEFAULT '', "
      "driver TEXT NOT NULL DEFAULT 'tapo' "
      "CHECK (driver IN ('tapo', 'onvif', 'rtsp')), "
      "icon TEXT NOT NULL DEFAULT 'video', "
      "record_mode TEXT NOT NULL DEFAULT 'events' "
      "CHECK (record_mode IN ('events', 'continuous')), "
      "retention_days INTEGER, capabilities TEXT NOT NULL DEFAULT '[]', "
      "config TEXT NOT NULL DEFAULT '{}', "
      "is_enabled INTEGER NOT NULL DEFAULT 1 CHECK (is_enabled IN (0, 1)), "
      "is_online INTEGER NOT NULL DEFAULT 0, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  client->execSqlSync(
      "INSERT INTO camera (id, name, ip, driver) "
      "VALUES (1, 'Talk Cam', '127.0.0.1', 'tapo')");
}

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

Json::Value body(const drogon::HttpResponsePtr& response)
{
  const auto json = response->getJsonObject();
  REQUIRE(json);
  return *json;
}

Json::Value talkBody(const std::string& text)
{
  Json::Value json;
  json["text"] = text;
  json["lang"] = "es";
  return json;
}

} // namespace

TEST_CASE("the camera-talk route synthesizes over the argus-tts wire")
{
  seedCameraDb();
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kCameraDb, "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  FakeTtsServer server;
  ConfigService::setRuntimeString(
      "tts.remote_url", "http://127.0.0.1:" + std::to_string(server.port()));

  auto speaker = std::make_shared<StubSpeaker>();
  CameraDriverTestAccess::install(1, speaker);

  CameraControlFeatureService service;
  const auto result =
      drogon::sync_wait(service.speak(1, CameraTalkDto::fromJson(
                                             talkBody("Hola camera"))));
  REQUIRE(result);
  CHECK(result->ok);

  const auto requests = server.requests();
  CHECK(requests.at("POST /tts/v1/synthesize") == 1);
  CHECK(requests.at("GET /tts/v1/config") >= 1);
  CHECK(speaker->speakCalls == 1);
  CHECK(speaker->lastSamples == 8);
  CHECK(speaker->lastRate == 22050);

  CameraControlController controller;
  const auto response =
      drogon::sync_wait(controller.talk(
          drogon::HttpRequest::newHttpJsonRequest(talkBody("Hola camera")), 1));
  const Json::Value envelope = body(response);
  CHECK(envelope["status"].asInt() == 200);
  CHECK(envelope["errors"].isNull());

  const auto missing = drogon::sync_wait(controller.talk(
      drogon::HttpRequest::newHttpJsonRequest(talkBody("Hola camera")), 99));
  CHECK(body(missing)["status"].asInt() == 404);
  CHECK(body(missing)["errors"]["code"] == "NOT_FOUND");

  ConfigService::setRuntimeString("tts.remote_url",
                                  "http://127.0.0.1:" +
                                      std::to_string(deadPort()));
  const auto dead = drogon::sync_wait(controller.talk(
      drogon::HttpRequest::newHttpJsonRequest(talkBody("Hola camera")), 1));
  const Json::Value deadEnvelope = body(dead);
  CHECK(deadEnvelope["status"].asInt() == 502);
  CHECK(deadEnvelope["errors"]["code"] == "CAMERA_UNREACHABLE");
  CHECK(deadEnvelope["errors"]["message"].asString().find(
            "Text-to-speech unavailable") != std::string::npos);
  CHECK(speaker->speakCalls == 2);

  ConfigService::setRuntimeString("tts.remote_url", "");

  drogon::app().quit();
  runner.join();
  std::remove(kCameraDb);
}
