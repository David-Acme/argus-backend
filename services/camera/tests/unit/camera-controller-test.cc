#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/media/camera-media-service.hxx>
#include <feature/monitor/camera-presence.hxx>
#include <drogon/WebSocketConnection.h>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/camera-control/controllers/camera-control-controller.hxx>
#include <feature/camera/controllers/camera-controller.hxx>
#include <feature/camera-control/dtos/camera-ptz-dto.hxx>
#include <feature/camera/dtos/create-camera-dto.hxx>
#include <feature/camera/dtos/update-camera-dto.hxx>
#include <feature/camera/services/camera-probe-service.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/services/camera-driver/stream-only-driver.hxx>
#include <shared/services/stream/snapshot-store.hxx>
#include <feature/zone/controllers/zone-controller.hxx>
#include <feature/zone/dtos/create-zone-dto.hxx>
#include <auth/jwt-filter.hxx>
#include <text/json-util.hxx>
#include <validation/validator.hxx>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
constexpr const char* kCameraDb = "camera-controller-test.db";

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

DbHandle openFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(path.c_str(), &raw) == SQLITE_OK)
    return {raw, sqlite3_close_v2};
  const std::string message =
      raw != nullptr ? std::string(sqlite3_errmsg(raw)) : std::string("failed");
  sqlite3_close_v2(raw);
  throw std::runtime_error("sqlite3_open " + path + ": " + message);
}

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) == SQLITE_OK) {
    sqlite3_free(error);
    return;
  }
  const std::string message =
      error != nullptr ? std::string(error) : std::string("failed");
  sqlite3_free(error);
  throw std::runtime_error("sqlite3_exec: " + message);
}

void seedCameraDb(const char* path)
{
  std::remove(path);
  const auto db = openFile(path);
  exec(db.get(),
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
      "tls_fingerprint TEXT NOT NULL DEFAULT '', "
      "tapo_secure INTEGER NOT NULL DEFAULT 0, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  exec(db.get(),
      "CREATE TABLE camera_stream ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "camera_id INTEGER NOT NULL REFERENCES camera(id) ON DELETE CASCADE, "
      "label TEXT NOT NULL DEFAULT '', url TEXT NOT NULL, "
      "resolution TEXT NOT NULL DEFAULT '', fps INTEGER NOT NULL DEFAULT 0, "
      "codec TEXT NOT NULL DEFAULT '', is_primary INTEGER NOT NULL DEFAULT 1, "
      "is_enabled INTEGER NOT NULL DEFAULT 1, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER, deleted_at INTEGER)");
  exec(db.get(),
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

class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
};

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

struct Refusal
{
  int status;
  std::string code;
  std::string message;
};

std::optional<Refusal> refusalOf(drogon::Task<drogon::HttpResponsePtr> task)
{
  try {
    drogon::sync_wait(std::move(task));
  }
  catch (const ResponseException& error) {
    return Refusal{error.statusCode(), error.errorCode(),
                   std::string(error.what())};
  }
  return std::nullopt;
}

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
}

TEST_CASE("camera and zone contracts hold on the argus-camera surface")
{
  std::filesystem::remove_all("/tmp/argus-camera-controller-test-upload");
  seedCameraDb(kCameraDb);
  drogon::app().setUploadPath("/tmp/argus-camera-controller-test-upload");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kCameraDb, "default", -1});

  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  CameraController cameraController;

  Json::Value createBody;
  createBody["name"] = "Contract Cam";
  createBody["ip"] = "192.168.1.30";
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
  CHECK(cam["ip"] == "192.168.1.30");
  CHECK(cam["port"].asInt() == 554);
  CHECK(cam["driver"] == "tapo");
  CHECK(cam["username"] == "");
  CHECK(cam["recordMode"] == "events");
  CHECK(cam["retentionDays"].asInt64() == 7);
  CHECK(cam["capabilities"] ==
        R"(["ptz","presets","microphone","privacy","led","dayNight","motion","autoTrack","alarm","sdCard"])");
  CHECK(cam["config"] == "{}");
  CHECK(cam["isEnabled"].asBool());
  CHECK_FALSE(cam["isOnline"].asBool());
  CHECK(cam["updatedAt"].isNull());
  CHECK(cam["deletedAt"].isNull());
  CHECK(cam.getMemberNames().size() == 19);
  checkNoCredentials(cam);

  Json::Value toRtsp;
  toRtsp["driver"] = "rtsp";
  const auto streamOnly = drogon::sync_wait(
      cameraController.update(drogon::HttpRequest::newHttpJsonRequest(toRtsp), cameraId));
  CHECK(body(streamOnly)["info"]["capabilities"] == R"(["streamOnly"])");
  Json::Value toTapo;
  toTapo["driver"] = "tapo";
  toTapo["cloudPassword"] = "cloud-secret";
  const auto talking = drogon::sync_wait(
      cameraController.update(drogon::HttpRequest::newHttpJsonRequest(toTapo), cameraId));
  CHECK(body(talking)["info"]["capabilities"].asString().find("\"talk\"") != std::string::npos);
  checkNoCredentials(body(talking)["info"]);
  {
    const auto stored = drogon::sync_wait(CameraRepository().findById(cameraId));
    REQUIRE(stored);
    CHECK(stored->cloudPassword == "cloud-secret");
  }
  Json::Value moved;
  moved["ip"] = "192.168.1.31";
  const auto relocated = drogon::sync_wait(
      cameraController.update(drogon::HttpRequest::newHttpJsonRequest(moved), cameraId));
  CHECK(body(relocated)["info"]["ip"] == "192.168.1.31");
  {
    const auto stored = drogon::sync_wait(CameraRepository().findById(cameraId));
    REQUIRE(stored);
    CHECK(stored->cloudPassword.empty());
    CHECK(stored->password.empty());
    CHECK(stored->tlsFingerprint.empty());
  }
  Json::Value tooLong;
  tooLong["retentionDays"] = Json::Int64(90);
  const auto refusedRetention = refusalOf(
      cameraController.update(drogon::HttpRequest::newHttpJsonRequest(tooLong), cameraId));
  REQUIRE(refusedRetention);
  CHECK(refusedRetention->status == 422);
  tooLong["retentionIncident"] = true;
  const auto incident = drogon::sync_wait(
      cameraController.update(drogon::HttpRequest::newHttpJsonRequest(tooLong), cameraId));
  CHECK(body(incident)["info"]["retentionDays"].asInt64() == 90);
  Json::Value closedIncident;
  closedIncident["retentionIncident"] = false;
  const auto capped = drogon::sync_wait(
      cameraController.update(drogon::HttpRequest::newHttpJsonRequest(closedIncident), cameraId));
  CHECK(body(capped)["info"]["retentionDays"].asInt64() == 60);

  const auto ptz = [](const char* body) { return CameraPtzDto::fromJson(json_util::fromString(body)); };
  CHECK(ptz(R"({"x":-10,"y":0})").x == -10);
  CHECK(ptz(R"({"angle":180})").angle == 180);
  CHECK(ptz(R"({"stop":true})").stop);
  CHECK_THROWS_AS(ptz(R"({"x":10})"), ValidationException);
  CHECK_THROWS_AS(ptz(R"({"x":10,"y":0,"stop":true})"), ValidationException);
  CHECK_THROWS_AS(ptz(R"({"x":400,"y":0})"), ValidationException);
  CHECK_THROWS_AS(ptz("{}"), ValidationException);

  auto updateReq = drogon::HttpRequest::newHttpJsonRequest(createBody);
  const auto missingUpdate =
      refusalOf(cameraController.update(updateReq, 999));
  if (!missingUpdate) {
    FAIL("expected a value in missingUpdate");
    return;
  }
  CHECK(missingUpdate->status == 404);
  CHECK(missingUpdate->code == "NOT_FOUND");
  CHECK(missingUpdate->message == "Camera not found");

  const auto removed = drogon::sync_wait(cameraController.remove(nullptr,
                                                                 cameraId));
  const Json::Value removedJson = body(removed);
  CHECK(removedJson["status"].asInt() == 200);
  CHECK(removedJson["info"]["deleted"].asBool());
  const auto removedTwice =
      refusalOf(cameraController.remove(nullptr, cameraId));
  if (!removedTwice) {
    FAIL("expected a value in removedTwice");
    return;
  }
  CHECK(removedTwice->status == 404);

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
  const auto ghostZone = refusalOf(zoneController.create(ghostReq));
  if (!ghostZone) {
    FAIL("expected a value in ghostZone");
    return;
  }
  CHECK(ghostZone->status == 404);
  CHECK(ghostZone->code == "NOT_FOUND");
  CHECK(ghostZone->message == "Camera not found");

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
  const auto zoneGoneTwice = refusalOf(zoneController.remove(nullptr, zoneId));
  if (!zoneGoneTwice) {
    FAIL("expected a value in zoneGoneTwice");
    return;
  }
  CHECK(zoneGoneTwice->status == 404);
  CHECK(zoneGoneTwice->code == "NOT_FOUND");
  CHECK(zoneGoneTwice->message == "Zone not found");

  CameraMediaService mediaService;
  const auto conn = std::make_shared<RecordingConnection>();
  conn->setContext(std::make_shared<JwtContext>(
      JwtContext{.sub = 1, .name = "Golden", .role = UserRole::Owner, .isActive = true, .deviceHash = {}, .sessionId = {}}));

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
    drogon::sync_wait(mediaService.handleText(
        {.conn = conn,
         .message = subscribeMessage(0),
         .raw = std::string_view{}}));
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
    drogon::sync_wait(mediaService.handleText(
        {.conn = conn,
         .message = subscribeMessage(999),
         .raw = std::string_view{}}));
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
    drogon::sync_wait(mediaService.handleText(
        {.conn = conn,
         .message = subscribeMessage(cameraId2),
         .raw = std::string_view{}}));
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
  const bool ackHandled = drogon::sync_wait(mediaService.handleText(
      {.conn = conn, .message = ackMessage, .raw = std::string_view{}}));
  CHECK(ackHandled);

  Json::Value unsubMessage;
  unsubMessage["type"] = "camera:unsubscribe";
  unsubMessage["payload"]["subId"] = 1;
  const bool unsubHandled = drogon::sync_wait(mediaService.handleText(
      {.conn = conn, .message = unsubMessage, .raw = std::string_view{}}));
  CHECK(unsubHandled);

  Json::Value unknownMessage;
  unknownMessage["type"] = "voice:start";
  unknownMessage["payload"] = Json::Value(Json::objectValue);
  const bool voiceIgnored = drogon::sync_wait(mediaService.handleText(
      {.conn = conn, .message = unknownMessage, .raw = std::string_view{}}));
  CHECK_FALSE(voiceIgnored);

  CameraControlController controlController;
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  SnapshotStore::instance().putFrame({.cameraId = cameraId2, .jpeg = "jpeg", .atMs = stamp});
  const auto snapshot = drogon::sync_wait(controlController.snapshot(nullptr, cameraId2));
  const Json::Value picture = body(snapshot)["info"];
  CHECK(picture["image"].asString() == "data:image/jpeg;base64,anBlZw==");
  CHECK(picture["capturedAt"].asInt64() == stamp);
  const auto missingSnapshot = refusalOf(controlController.snapshot(nullptr, 999));
  if (!missingSnapshot) {
    FAIL("expected a value in missingSnapshot");
    return;
  }
  CHECK(missingSnapshot->status == 404);
  SnapshotStore::instance().forget(cameraId2);

  CameraPresenceRecorder presence;
  const CameraRepository cameras;
  const auto onlineOf = [&cameras](int64_t id) {
    const auto row = drogon::sync_wait(cameras.findById(id));
    REQUIRE(row.has_value());
    return row.value_or(CameraSchema{}).isOnline;
  };
  CHECK_FALSE(onlineOf(cameraId2));
  drogon::sync_wait(presence.record({.cameraId = cameraId2, .reachable = true}));
  CHECK(onlineOf(cameraId2));
  drogon::sync_wait(presence.record({.cameraId = cameraId2, .reachable = false}));
  CHECK(onlineOf(cameraId2));
  drogon::sync_wait(presence.record({.cameraId = cameraId2, .reachable = false}));
  CHECK_FALSE(onlineOf(cameraId2));
  drogon::sync_wait(presence.record({.cameraId = cameraId2, .reachable = true}));
  CHECK(onlineOf(cameraId2));
  drogon::sync_wait(presence.record({.cameraId = 424242, .reachable = true}));

  std::remove(kCameraDb);
  std::remove((std::string(kCameraDb) + "-wal").c_str());
  std::remove((std::string(kCameraDb) + "-shm").c_str());
  std::filesystem::remove_all("/tmp/argus-camera-controller-test-upload");
}

TEST_CASE("a camera address must be a literal IP and a record mode a known one")
{
  const auto refusesField = [](const auto& parse, const char* field) {
    try {
      static_cast<void>(parse());
    }
    catch (const ValidationException& e) {
      return e.errors().count(field) == 1;
    }
    return false;
  };
  Json::Value create;
  create["name"] = "Door";
  create["ip"] = "10.evil.example";
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "ip"));
  create["ip"] = "10.0.0.1@attacker.example";
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "ip"));
  create["ip"] = "192.168.1.40";
  CHECK(CreateCameraDto::fromJson(create).ip == "192.168.1.40");

  Json::Value update;
  update["ip"] = "camera.local";
  CHECK(refusesField([&] { return UpdateCameraDto::fromJson(update); }, "ip"));
  update = Json::Value(Json::objectValue);
  update["recordMode"] = "always";
  CHECK(refusesField([&] { return UpdateCameraDto::fromJson(update); },
                     "recordMode"));
  update["recordMode"] = "continuous";
  CHECK(UpdateCameraDto::fromJson(update).recordMode == "continuous");
}

TEST_CASE("a camera lives on the local network and its stream paths are plain")
{
  const auto refusesField = [](const auto& parse, const char* field) {
    try {
      static_cast<void>(parse());
    }
    catch (const ValidationException& e) {
      return e.errors().count(field) == 1;
    }
    return false;
  };
  Json::Value create;
  create["name"] = "Gate";
  create["ip"] = "8.8.8.8";
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "ip"));
  create["ip"] = "fd12:3456::7";
  CHECK(CreateCameraDto::fromJson(create).ip == "fd12:3456::7");
  create["ip"] = "192.168.1.60";
  create["streamPath"] = "Streaming/Channels/101";
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "streamPath"));
  create["streamPath"] = "/cam/realmonitor?channel=1&subtype=0";
  create["subStreamPath"] = "/live 2";
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "subStreamPath"));
  create["subStreamPath"] = "/Streaming/Channels/102";
  const auto parsed = CreateCameraDto::fromJson(create);
  CHECK(parsed.streamPath == "/cam/realmonitor?channel=1&subtype=0");
  CHECK(parsed.subStreamPath == "/Streaming/Channels/102");
  create["retentionDays"] = static_cast<Json::Int64>(-1);
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "retentionDays"));
  create["retentionDays"] = static_cast<Json::Int64>(61);
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "retentionDays"));
  create["retentionIncident"] = true;
  CHECK(CreateCameraDto::fromJson(create).retentionDays == 61);
  create["retentionDays"] = static_cast<Json::Int64>(121);
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "retentionDays"));
  create.removeMember("retentionIncident");
  create["retentionDays"] = static_cast<Json::Int64>(30);
  create["ip"] = "127.0.0.1";
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "ip"));
  create["ip"] = "169.254.169.254";
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "ip"));
  create["ip"] = "::1";
  CHECK(refusesField([&] { return CreateCameraDto::fromJson(create); }, "ip"));
  create["ip"] = "192.168.1.60";

  Json::Value update;
  update["port"] = 0;
  CHECK(refusesField([&] { return UpdateCameraDto::fromJson(update); }, "port"));
  update["port"] = 70000;
  CHECK(refusesField([&] { return UpdateCameraDto::fromJson(update); }, "port"));
  update["port"] = 8554;
  update["ip"] = "203.0.113.9";
  CHECK(refusesField([&] { return UpdateCameraDto::fromJson(update); }, "ip"));
  update["ip"] = "10.1.2.3";
  update["model"] = std::string(81, 'm');
  CHECK(refusesField([&] { return UpdateCameraDto::fromJson(update); }, "model"));
  update["model"] = "C225";
  update["streamPath"] = "";
  const auto cleared = UpdateCameraDto::fromJson(update);
  CHECK(cleared.port == 8554);
  CHECK(cleared.streamPath == "");
}

TEST_CASE("a removed camera leaves nothing behind in the snapshot store")
{
  SnapshotStore::instance().putFrame({.cameraId = 4242, .jpeg = "jpeg-bytes", .atMs = 1});
  SnapshotStore::instance().putPersonCrop(
      {.cameraId = 4242, .trackId = 7, .jpeg = "crop-bytes", .atMs = 1});
  REQUIRE(SnapshotStore::instance().frame(4242).has_value());
  SnapshotStore::instance().forget(4242);
  CHECK_FALSE(SnapshotStore::instance().frame(4242).has_value());
  CHECK_FALSE(SnapshotStore::instance().latestPersonCrop(4242).has_value());
}

TEST_CASE("a stream sink admits one box larger than its window when idle")
{
  CameraStreamSink sink(nullptr, 100);
  CHECK(sink.tryReserve(500));
  CHECK_FALSE(sink.tryReserve(1));
  sink.release(450);
  CHECK(sink.tryReserve(50));
  CHECK_FALSE(sink.tryReserve(1));
  sink.release(100);
  CHECK(sink.tryReserve(100));
}

TEST_CASE("a stream-only camera answers its capabilities with every control off")
{
  CameraSchema camera;
  camera.driver = CameraDriver::Rtsp;
  camera.model = "testsrc2";
  StreamOnlyDriver driver(camera);
  const Json::Value capabilities = driver.capabilities();
  for (const char* control : {"ptz", "presets", "talk", "privacy", "led", "alarm"})
    CHECK_FALSE(capabilities[control].asBool());
  CHECK(capabilities["streamOnly"].asBool());
  const DriverResult status = driver.status();
  CHECK(status.ok);
  CHECK(status.data["model"].asString() == "testsrc2");
  const DriverResult moved = driver.move({.x = std::nullopt, .y = std::nullopt, .angle = 90});
  CHECK_FALSE(moved.ok);
  CHECK(moved.error.find("stream video only") != std::string::npos);
}

TEST_CASE("a probe reuses stored secrets only against the stored address, one at a time per user")
{
  CameraSchema stored;
  stored.ip = "192.168.1.30";
  stored.port = 554;
  ProbeCameraDto body;
  body.ip = "192.168.1.30";
  body.port = 554;
  body.cameraId = 4;
  CHECK(camera_probe::reusesStoredSecrets(body));
  CHECK(camera_probe::storedAddressMatches({.body = body, .stored = stored}));
  body.ip = "192.168.1.99";
  CHECK_FALSE(camera_probe::storedAddressMatches({.body = body, .stored = stored}));
  body.ip = "192.168.1.30";
  body.port = 8554;
  CHECK_FALSE(camera_probe::storedAddressMatches({.body = body, .stored = stored}));
  body.password = "typed";
  body.cloudPassword = "typed-cloud";
  CHECK_FALSE(camera_probe::reusesStoredSecrets(body));

  ProbeSlots slots;
  auto first = slots.acquire(7);
  CHECK(first.has_value());
  CHECK_FALSE(slots.acquire(7).has_value());
  CHECK(slots.acquire(8).has_value());
  first.reset();
  CHECK(slots.acquire(7).has_value());
}

TEST_CASE("an ack only releases what its own socket was sent")
{
  CameraStreamSink sink(nullptr, 100);
  CHECK(sink.tryReserve(80));
  CHECK(sink.release(500) == 80);
  CHECK(sink.release(10) == 0);
  CHECK(sink.release(-5) == 0);
}
