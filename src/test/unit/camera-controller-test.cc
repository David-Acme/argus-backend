#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <controllers/camera-media-service.hxx>
#include <drogon/WebSocketConnection.h>
#include <drogon/drogon.h>
#include <feature/api/camera/controllers/camera-controller.hxx>
#include <feature/api/camera/dtos/create-camera-dto.hxx>
#include <feature/api/zone/controllers/zone-controller.hxx>
#include <feature/api/zone/dtos/create-zone-dto.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/validation/validator.hxx>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace
{
constexpr const char* kCameraDb = "camera-controller-test.db";

void seedCameraDb(const char* path)
{
  std::remove(path);
  auto client =
      drogon::orm::DbClient::newSqlite3Client(std::string("filename=") + path,
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
      "CREATE TABLE camera_stream ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "camera_id INTEGER NOT NULL REFERENCES camera(id) ON DELETE CASCADE, "
      "label TEXT NOT NULL DEFAULT '', url TEXT NOT NULL, "
      "resolution TEXT NOT NULL DEFAULT '', fps INTEGER NOT NULL DEFAULT 0, "
      "codec TEXT NOT NULL DEFAULT '', is_primary INTEGER NOT NULL DEFAULT 1, "
      "is_enabled INTEGER NOT NULL DEFAULT 1, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  client->execSqlSync(
      "CREATE TABLE zone ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "camera_id INTEGER NOT NULL REFERENCES camera(id) ON DELETE CASCADE, "
      "name TEXT NOT NULL, points TEXT NOT NULL, "
      "zone_type TEXT NOT NULL DEFAULT 'monitor' "
      "CHECK (zone_type IN ('monitor', 'alert', 'exclude')), "
      "color TEXT NOT NULL DEFAULT '#FF0000', "
      "is_enabled INTEGER NOT NULL DEFAULT 1, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
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

// The camera schema never exposes credentials: the sync/API DTO boundary
// carries username only.
void checkNoCredentials(const Json::Value& info)
{
  CHECK_FALSE(info.isMember("password"));
  CHECK_FALSE(info.isMember("cloudPassword"));
}

class RecordingConnection final : public drogon::WebSocketConnection
{
public:
  void send(const char* msg, uint64_t len,
            const drogon::WebSocketMessageType type) override
  {
    (void)type;
    messages.emplace_back(msg, len);
  }
  void send(std::string_view msg,
            const drogon::WebSocketMessageType type) override
  {
    (void)type;
    messages.emplace_back(msg);
  }
  void sendJson(const Json::Value& json,
                const drogon::WebSocketMessageType type) override
  {
    (void)type;
    messages.push_back(json_util::toString(json));
  }
  const trantor::InetAddress& localAddr() const override { return addr_; }
  const trantor::InetAddress& peerAddr() const override { return addr_; }
  bool connected() const override { return true; }
  bool disconnected() const override { return false; }
  void shutdown(const drogon::CloseCode, const std::string&) override {}
  void forceClose() override {}
  void setPingMessage(const std::string&,
                      const std::chrono::duration<double>&) override
  {
  }
  void disablePing() override {}

  std::vector<std::string> messages;

private:
  trantor::InetAddress addr_{"127.0.0.1", 0};
};
} // namespace

TEST_CASE("camera and zone contracts hold on the argus-camera surface")
{
  std::filesystem::remove_all("/tmp/argus-camera-controller-test-upload");
  seedCameraDb(kCameraDb);
  drogon::app().setUploadPath("/tmp/argus-camera-controller-test-upload");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kCameraDb, "default", -1});

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  // ── Camera CRUD envelopes ─────────────────────────────────────────────────
  CameraController cameraController;

  Json::Value createBody;
  createBody["name"] = "Contract Cam";
  createBody["ip"] = "127.0.0.1";
  createBody["port"] = 554;
  createBody["driver"] = "tapo";
  createBody["retentionDays"] = Json::Int64(7);
  auto createReq = drogon::HttpRequest::newHttpJsonRequest(createBody);
  const auto created = drogon::sync_wait(cameraController.create(createReq));
  REQUIRE(created);
  const Json::Value createdJson = body(created);
  CHECK(createdJson["status"].asInt() == 200);
  CHECK(createdJson["errors"].isNull());
  const Json::Value cam = createdJson["info"];
  const int64_t cameraId = cam["id"].asInt64();
  CHECK(cameraId > 0);
  CHECK(cam["name"] == "Contract Cam");
  CHECK(cam["ip"] == "127.0.0.1");
  CHECK(cam["port"].asInt() == 554);
  CHECK(cam["driver"] == "tapo");
  CHECK(cam["username"] == "");
  CHECK(cam["recordMode"] == "events");
  CHECK(cam["retentionDays"].asInt64() == 7);
  CHECK(cam["capabilities"] == "[]");
  CHECK(cam["config"] == "{}");
  CHECK(cam["isEnabled"].asBool());
  CHECK_FALSE(cam["isOnline"].asBool());
  CHECK(cam["updatedAt"].isNull());
  CHECK(cam["deletedAt"].isNull());
  // The wire shape is frozen: every field the legacy serves, nothing more.
  CHECK(cam.getMemberNames().size() == 19);
  checkNoCredentials(cam);

  auto updateReq = drogon::HttpRequest::newHttpJsonRequest(createBody);
  const auto missingUpdate =
      drogon::sync_wait(cameraController.update(updateReq, 999));
  const Json::Value missingUpdateJson = body(missingUpdate);
  CHECK(missingUpdateJson["status"].asInt() == 404);
  CHECK(missingUpdateJson["info"].isNull());
  CHECK(missingUpdateJson["errors"]["code"] == "NOT_FOUND");
  CHECK(missingUpdateJson["errors"]["message"] == "Camera not found");

  const auto removed = drogon::sync_wait(cameraController.remove(nullptr,
                                                                 cameraId));
  const Json::Value removedJson = body(removed);
  CHECK(removedJson["status"].asInt() == 200);
  CHECK(removedJson["info"]["deleted"].asBool());
  const auto removedTwice =
      drogon::sync_wait(cameraController.remove(nullptr, cameraId));
  CHECK(body(removedTwice)["status"].asInt() == 404);

  // DTO validation keeps the legacy 422 envelope contract.
  Json::Value invalidBody;
  invalidBody["name"] = "No Ip";
  bool validationThrown = false;
  try {
    const auto dto = CreateCameraDto::fromJson(invalidBody);
    (void)dto;
  }
  catch (const ValidationException& e) {
    validationThrown = true;
    CHECK(e.statusCode() == 422);
    REQUIRE(e.errors().count("ip") == 1);
  }
  CHECK(validationThrown);

  // ── Zone CRUD envelopes ───────────────────────────────────────────────────
  ZoneController zoneController;

  const auto reCam = drogon::sync_wait(cameraController.create(createReq));
  const int64_t cameraId2 = body(reCam)["info"]["id"].asInt64();

  Json::Value zoneBody;
  zoneBody["cameraId"] = Json::Int64(999);
  zoneBody["name"] = "Ghost Zone";
  zoneBody["points"] = Json::Value(Json::arrayValue);
  for (const auto& [x, y] :
       std::vector<std::pair<double, double>>{{0.1, 0.1}, {0.5, 0.1}, {0.5, 0.5}}) {
    Json::Value point;
    point["x"] = x;
    point["y"] = y;
    zoneBody["points"].append(point);
  }
  auto ghostReq = drogon::HttpRequest::newHttpJsonRequest(zoneBody);
  const auto ghostZone = drogon::sync_wait(zoneController.create(ghostReq));
  const Json::Value ghostJson = body(ghostZone);
  CHECK(ghostJson["status"].asInt() == 404);
  CHECK(ghostJson["info"].isNull());
  CHECK(ghostJson["errors"]["code"] == "NOT_FOUND");
  CHECK(ghostJson["errors"]["message"] == "Camera not found");

  zoneBody["cameraId"] = Json::Int64(cameraId2);
  auto zoneReq = drogon::HttpRequest::newHttpJsonRequest(zoneBody);
  const auto zoneCreated = drogon::sync_wait(zoneController.create(zoneReq));
  const Json::Value zoneJson = body(zoneCreated);
  CHECK(zoneJson["status"].asInt() == 200);
  CHECK(zoneJson["errors"].isNull());
  const Json::Value zone = zoneJson["info"];
  const int64_t zoneId = zone["id"].asInt64();
  CHECK(zoneId > 0);
  CHECK(zone["cameraId"].asInt64() == cameraId2);
  CHECK(zone["name"] == "Ghost Zone");
  CHECK(zone["zoneType"] == "monitor");
  CHECK(zone["color"] == "#FF0000");
  CHECK(zone["isEnabled"].asBool());
  CHECK(zone["updatedAt"].isNull());
  CHECK(zone["deletedAt"].isNull());
  CHECK(zone.getMemberNames().size() == 10);

  const auto zoneGone = drogon::sync_wait(zoneController.remove(nullptr,
                                                                zoneId));
  CHECK(body(zoneGone)["status"].asInt() == 200);
  const auto zoneGoneTwice =
      drogon::sync_wait(zoneController.remove(nullptr, zoneId));
  CHECK(body(zoneGoneTwice)["status"].asInt() == 404);

  // ── camera:subscribe decision path ────────────────────────────────────────
  // go2rtc is deliberately not started here: the known-camera subscribe must
  // degrade to the 503 go2rtc_not_running envelope, the unknown camera
  // to the 404 Camera not found envelope.
  CameraMediaService mediaService;
  const auto conn = std::make_shared<RecordingConnection>();
  conn->setContext(std::make_shared<JwtContext>(
      JwtContext{1, "Golden", UserRole::Owner, true}));

  auto subscribeMessage = [](int64_t cameraId) {
    Json::Value payload;
    payload["cameraId"] = Json::Int64(cameraId);
    payload["quality"] = "main";
    Json::Value message;
    message["type"] = "camera:subscribe";
    message["payload"] = payload;
    return message;
  };

  bool badRequestThrown = false;
  try {
    drogon::sync_wait(mediaService.forwardText(
        conn, subscribeMessage(0), std::string_view{}));
  }
  catch (const ResponseException& e) {
    badRequestThrown = true;
    CHECK(e.statusCode() == 400);
    CHECK(e.errorCode() == "BAD_REQUEST");
    CHECK(e.what() == std::string("Invalid cameraId"));
  }
  CHECK(badRequestThrown);

  bool notFoundThrown = false;
  try {
    drogon::sync_wait(mediaService.forwardText(
        conn, subscribeMessage(999), std::string_view{}));
  }
  catch (const ResponseException& e) {
    notFoundThrown = true;
    CHECK(e.statusCode() == 404);
    CHECK(e.errorCode() == "NOT_FOUND");
    CHECK(e.what() == std::string("Camera not found"));
  }
  CHECK(notFoundThrown);

  bool go2rtcThrown = false;
  try {
    drogon::sync_wait(mediaService.forwardText(
        conn, subscribeMessage(cameraId2), std::string_view{}));
  }
  catch (const ResponseException& e) {
    go2rtcThrown = true;
    CHECK(e.statusCode() == 503);
    CHECK(e.errorCode() == "SERVICE_UNAVAILABLE");
    CHECK(e.what() == std::string("go2rtc_not_running"));
  }
  CHECK(go2rtcThrown);

  Json::Value ackMessage;
  ackMessage["type"] = "camera:ack";
  ackMessage["payload"]["subId"] = 1;
  ackMessage["payload"]["bytes"] = Json::Int64(1024);
  const bool ackHandled = drogon::sync_wait(
      mediaService.forwardText(conn, ackMessage, std::string_view{}));
  CHECK(ackHandled);

  Json::Value unsubMessage;
  unsubMessage["type"] = "camera:unsubscribe";
  unsubMessage["payload"]["subId"] = 1;
  const bool unsubHandled = drogon::sync_wait(
      mediaService.forwardText(conn, unsubMessage, std::string_view{}));
  CHECK(unsubHandled);

  Json::Value unknownMessage;
  unknownMessage["type"] = "voice:start";
  unknownMessage["payload"] = Json::Value(Json::objectValue);
  const bool voiceIgnored = drogon::sync_wait(
      mediaService.forwardText(conn, unknownMessage, std::string_view{}));
  CHECK_FALSE(voiceIgnored);

  drogon::app().quit();
  runner.join();

  std::remove(kCameraDb);
  std::filesystem::remove_all("/tmp/argus-camera-controller-test-upload");
}
