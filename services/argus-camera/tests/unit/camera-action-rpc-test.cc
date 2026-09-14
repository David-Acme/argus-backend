#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "fake-stt-server.hxx"
#include "fake-tts-server.hxx"

#include <arpa/inet.h>
#include <atomic>
#include <barrier>
#include <camera/camera-action-client.hxx>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <feature/actions/audio-capture.hxx>
#include <feature/actions/camera-action-rpc-service.hxx>
#include <fstream>
#include <functional>
#include <grpcpp/grpcpp.h>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <shared/repositories/action-command/action-command-repository.hxx>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/stream/snapshot-store.hxx>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kCameraDb = "camera-action-rpc-test.db";
constexpr const char* kConfig = "camera-action-rpc-test.toml";
constexpr const char* kFleet = "fleet-test";
constexpr const char* kGuardCredential = "guard-camera-cred";

struct StubActuator final : ICameraDriver
{
  std::atomic<int> speakCalls{0};
  std::atomic<size_t> lastSamples{0};
  bool settingsCalled{false};
  std::optional<bool> alarm;
  std::optional<int> alarmVolume;
  std::mutex speakMutex;
  std::condition_variable speakCv;
  bool blockSpeak{false};
  bool speakReleased{false};
  bool throwOnSpeak{false};
  bool throwOnSettings{false};
  bool blockSettings{false};
  bool settingsReleased{false};
  std::mutex settingsMutex;
  std::condition_variable settingsCv;

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
  DriverResult settings(const DriverSettingsInput& input) override
  {
    settingsCalled = true;
    alarm = input.alarm;
    alarmVolume = input.alarmVolume;
    if (blockSettings) {
      std::unique_lock lock(settingsMutex);
      settingsCv.wait(lock, [this]() { return settingsReleased; });
    }
    if (throwOnSettings)
      throw std::runtime_error("driver settings exploded");
    return DriverResult{.ok = true, .error = "", .data = Json::Value()};
  }

  void releaseSettings()
  {
    {
      std::lock_guard lock(settingsMutex);
      settingsReleased = true;
    }
    settingsCv.notify_all();
  }
  DriverResult speak(const DriverSpeakInput& input) override
  {
    lastSamples.store(input.samples.size());
    speakCalls.fetch_add(1);
    if (blockSpeak) {
      std::unique_lock lock(speakMutex);
      speakCv.wait(lock, [this]() { return speakReleased; });
    }
    if (throwOnSpeak)
      throw std::runtime_error("driver speak exploded");
    return DriverResult{.ok = true, .error = "", .data = Json::Value()};
  }

  void releaseSpeak()
  {
    {
      std::lock_guard lock(speakMutex);
      speakReleased = true;
    }
    speakCv.notify_all();
  }
};

// A bound-then-closed port: the gRPC server can rebind it immediately.
int freePort()
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

void writeConfig(bool actionsEnabled)
{
  std::ofstream out(kConfig, std::ios::trunc);
  out << "[actions]\nenabled = " << (actionsEnabled ? "true" : "false")
      << "\n\n[identity]\nrpc_secret = \"" << kFleet
      << "\"\n\n[grpc]\ncaller_guard = \"" << kGuardCredential << "\"\n";
}

void seedCameraDb()
{
  std::remove(kCameraDb);
  std::remove((std::string(kCameraDb) + "-wal").c_str());
  std::remove((std::string(kCameraDb) + "-shm").c_str());
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
  client->execSqlSync("INSERT INTO camera (id, name, ip, driver) "
                      "VALUES (1, 'Action Cam', '127.0.0.1', 'tapo')");
  client->execSqlSync(
      "CREATE TABLE action_command ("
      "command_id TEXT NOT NULL PRIMARY KEY, "
      "kind TEXT NOT NULL DEFAULT '', camera_id INTEGER NOT NULL DEFAULT 0, "
      "status TEXT NOT NULL DEFAULT 'claimed', "
      "detail TEXT NOT NULL DEFAULT '', "
      "response TEXT NOT NULL DEFAULT '', "
      "fingerprint TEXT NOT NULL DEFAULT '', "
      "attempts INTEGER NOT NULL DEFAULT 0, "
      "generation INTEGER NOT NULL DEFAULT 0, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "updated_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  client->execSqlSync(
      "CREATE TABLE siren_lease ("
      "camera_id INTEGER NOT NULL PRIMARY KEY, "
      "command_id TEXT NOT NULL DEFAULT '', "
      "expires_at INTEGER NOT NULL, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
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
} // namespace

TEST_CASE("the camera action RPC drives the driver behind the fleet gate")
{
  seedCameraDb();
  writeConfig(true);
  ConfigService::load(kConfig);
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kCameraDb, "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  auto actuator = std::make_shared<StubActuator>();
  CameraDriverTestAccess::install(1, actuator);
  SnapshotStore::instance().putPersonCrop(1, 7, "jpeg-bytes", 1234);

  FakeTtsServer tts;
  ConfigService::setRuntimeString("tts.remote_url",
                                  "http://127.0.0.1:" +
                                      std::to_string(tts.port()));

  const int port = freePort();
  CameraActionRpcService service(
      {.callers = {argus::sdk::CallerCredential{.service = "argus-guard",
                                                .secret = kGuardCredential}},
       .transcriber = makeHttpSttTranscriber()});
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:" + std::to_string(port),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  REQUIRE(server);

  CameraActionClient client({.target = "127.0.0.1:" + std::to_string(port),
                             .credential = kGuardCredential});

  const auto crop =
      client.personCrop({.cameraId = 1, .trackId = 7, .firstSeenMs = 0});
  REQUIRE(crop);
  CHECK(crop->jpeg == "jpeg-bytes");
  CHECK(crop->capturedAt == 1234);
  CHECK_FALSE(
      client.personCrop({.cameraId = 1, .trackId = 8, .firstSeenMs = 0}));

  const auto announced = client.announce({.cameraId = 1,
                                          .text = "Hola",
                                          .lang = "es",
                                          .commandId = "cmd-announce-1",
                                          .encounterId = 9,
                                          .expiresAt = 0});
  CHECK(announced.succeeded());
  CHECK_FALSE(announced.duplicate);
  CHECK(actuator->speakCalls == 1);
  CHECK(tts.requests().at("POST /tts/v1/synthesize") == 1);

  const auto repeated = client.announce({.cameraId = 1,
                                         .text = "Hola",
                                         .lang = "es",
                                         .commandId = "cmd-announce-1",
                                         .encounterId = 9,
                                         .expiresAt = 0});
  CHECK(repeated.succeeded());
  CHECK(repeated.duplicate);
  CHECK(actuator->speakCalls == 1);

  const auto expiredReplay = client.announce({.cameraId = 1,
                                              .text = "Hola",
                                              .lang = "es",
                                              .commandId = "cmd-announce-1",
                                              .encounterId = 9,
                                              .expiresAt = 1});
  CHECK(expiredReplay.succeeded());
  CHECK(expiredReplay.duplicate);

  const auto fingerprintConflict =
      client.announce({.cameraId = 1,
                       .text = "Distinto",
                       .lang = "es",
                       .commandId = "cmd-announce-1",
                       .encounterId = 9,
                       .expiresAt = 0});
  CHECK(fingerprintConflict.conflict());
  CHECK_FALSE(fingerprintConflict.succeeded());
  CHECK(actuator->speakCalls == 1);

  const auto expired = client.announce({.cameraId = 1,
                                        .text = "Hola",
                                        .lang = "es",
                                        .commandId = "cmd-expired",
                                        .encounterId = 9,
                                        .expiresAt = 1});
  CHECK_FALSE(expired.succeeded());
  CHECK(expired.outcome == argus::camera::v1::CommandOutcome::REJECTED);
  CHECK(expired.detail == "expired");
  CHECK(actuator->speakCalls == 1);

  const auto alarmed = client.alarm({.cameraId = 1,
                                     .seconds = 2,
                                     .commandId = "cmd-alarm-1",
                                     .encounterId = 9,
                                     .expiresAt = 0});
  CHECK(alarmed.succeeded());
  CHECK(actuator->speakCalls == 2);
  CHECK(actuator->lastSamples == 32000);

  const auto siren = client.setSiren({.cameraId = 1,
                                      .enabled = true,
                                      .commandId = "cmd-siren-1",
                                      .encounterId = 9,
                                      .expiresAt = 0,
                                      .leaseSeconds = 20});
  CHECK(siren.succeeded());
  CHECK(actuator->settingsCalled);
  CHECK(actuator->alarm == true);
  REQUIRE(actuator->alarmVolume);
  CHECK(*actuator->alarmVolume == 100);

  CameraActionClient badClient(
      {.target = "127.0.0.1:" + std::to_string(port), .credential = "wrong"});
  CHECK(badClient
            .setSiren({.cameraId = 1,
                       .enabled = false,
                       .commandId = "cmd-bad-client",
                       .encounterId = 0,
                       .expiresAt = 0,
                       .leaseSeconds = 0})
            .rejected());

  CameraActionClient impostorClient(
      {.target = "127.0.0.1:" + std::to_string(port), .credential = kFleet});
  CHECK(impostorClient
            .announce({.cameraId = 1,
                       .text = "Hola",
                       .lang = "es",
                       .commandId = "cmd-rogue",
                       .encounterId = 9,
                       .expiresAt = 0})
            .rejected());

  CHECK(client
            .alarm({.cameraId = 99,
                    .seconds = 1,
                    .commandId = "cmd-missing-camera",
                    .encounterId = 0,
                    .expiresAt = 0})
            .rejected());

  const auto listen = client.listen({.cameraId = 1,
                                     .seconds = 1,
                                     .lang = "es",
                                     .commandId = "cmd-listen",
                                     .encounterId = 9});
  CHECK_FALSE(listen.succeeded());
  CHECK_FALSE(listen.captured);
  CHECK(listen.text.empty());

  ActionCommandRepository commands;
  const auto claimed =
      drogon::sync_wait(commands.claim({.commandId = "cmd-listen-replay",
                                        .kind = "greet_listen",
                                        .cameraId = 1,
                                        .fingerprint = {},
                                        .at = 100}));
  CHECK(claimed.kind == ActionClaimKind::New);
  CHECK(drogon::sync_wait(
      commands.settle({.commandId = "cmd-listen-replay",
                       .status = "succeeded",
                       .detail = "captured",
                       .response = "{\"text\":\"hola\",\"captured\":true}",
                       .at = 101})));
  const auto replayed =
      drogon::sync_wait(commands.claim({.commandId = "cmd-listen-replay",
                                        .kind = "greet_listen",
                                        .cameraId = 1,
                                        .fingerprint = {},
                                        .at = 102}));
  CHECK(replayed.kind == ActionClaimKind::Completed);
  CHECK(replayed.response.find("hola") != std::string::npos);

  auto blocking = std::make_shared<StubActuator>();
  blocking->blockSpeak = true;
  blocking->speakReleased = false;
  CameraDriverTestAccess::install(1, blocking);
  CameraCommandResult fencedAck;
  std::thread firstCall([&]() {
    fencedAck = client.announce({.cameraId = 1,
                                 .text = "Fence",
                                 .lang = "es",
                                 .commandId = "cmd-fence",
                                 .encounterId = 9,
                                 .expiresAt = 0});
  });
  while (blocking->speakCalls.load() == 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto inFlight = client.announce({.cameraId = 1,
                                         .text = "Fence",
                                         .lang = "es",
                                         .commandId = "cmd-fence",
                                         .encounterId = 9,
                                         .expiresAt = 0});
  CHECK(inFlight.inFlight());
  CHECK_FALSE(inFlight.succeeded());
  CHECK(inFlight.detail == "in_flight");
  CHECK(blocking->speakCalls.load() == 1);
  blocking->releaseSpeak();
  firstCall.join();
  CHECK(fencedAck.succeeded());
  const auto fenceReplay = client.announce({.cameraId = 1,
                                            .text = "Fence",
                                            .lang = "es",
                                            .commandId = "cmd-fence",
                                            .encounterId = 9,
                                            .expiresAt = 0});
  CHECK(fenceReplay.succeeded());
  CHECK(fenceReplay.duplicate);
  CHECK(blocking->speakCalls.load() == 1);

  FakeSttServer stt;
  ConfigService::setRuntimeString("stt.remote_url",
                                  "http://127.0.0.1:" +
                                      std::to_string(stt.port()));
  int captureCalls = 0;
  audio_capture::setCaptureFunctionForTest(
      [&captureCalls](const AudioCaptureInput&) {
        ++captureCalls;
        AudioCaptureResult result;
        result.ok = true;
        result.speechDetected = true;
        result.endpointed = true;
        result.samples.assign(16000, 0);
        return result;
      });
  const auto listenFull = client.listen({.cameraId = 1,
                                         .seconds = 1,
                                         .lang = "es",
                                         .commandId = "cmd-listen-full",
                                         .encounterId = 9,
                                         .expiresAt = 0});
  CHECK(listenFull.succeeded());
  CHECK(listenFull.captured);
  CHECK(listenFull.speechDetected);
  CHECK(listenFull.endpointed);
  CHECK_FALSE(listenFull.duplicate);
  REQUIRE_FALSE(listenFull.text.empty());
  const std::string transcribed = listenFull.text;
  CHECK(captureCalls == 1);
  const auto listenReplay = client.listen({.cameraId = 1,
                                           .seconds = 1,
                                           .lang = "es",
                                           .commandId = "cmd-listen-full",
                                           .encounterId = 9,
                                           .expiresAt = 0});
  CHECK(listenReplay.duplicate);
  CHECK(listenReplay.text == transcribed);
  CHECK(listenReplay.speechDetected);
  CHECK(captureCalls == 1);

  std::mutex captureMutex;
  std::condition_variable captureCv;
  bool captureReleased = false;
  int blockingCaptures = 0;
  audio_capture::setCaptureFunctionForTest([&](const AudioCaptureInput&) {
    {
      std::lock_guard lock(captureMutex);
      ++blockingCaptures;
    }
    {
      std::unique_lock lock(captureMutex);
      captureCv.wait(lock, [&captureReleased]() { return captureReleased; });
    }
    AudioCaptureResult result;
    result.ok = true;
    result.speechDetected = true;
    result.samples.assign(16000, 0);
    return result;
  });
  CameraCommandResult firstListen;
  std::thread listenThread([&]() {
    firstListen = client.listen({.cameraId = 1,
                                 .seconds = 1,
                                 .lang = "es",
                                 .commandId = "cmd-listen-fence",
                                 .encounterId = 9,
                                 .expiresAt = 0});
  });
  const auto blockedOnce = [&]() {
    std::lock_guard lock(captureMutex);
    return blockingCaptures > 0;
  };
  while (!blockedOnce())
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto inFlightListen = client.listen({.cameraId = 1,
                                             .seconds = 1,
                                             .lang = "es",
                                             .commandId = "cmd-listen-fence",
                                             .encounterId = 9,
                                             .expiresAt = 0});
  CHECK(inFlightListen.inFlight());
  {
    std::lock_guard lock(captureMutex);
    CHECK(blockingCaptures == 1);
    captureReleased = true;
  }
  captureCv.notify_all();
  listenThread.join();
  CHECK(firstListen.captured);
  const auto fenceListenReplay = client.listen({.cameraId = 1,
                                                .seconds = 1,
                                                .lang = "es",
                                                .commandId = "cmd-listen-fence",
                                                .encounterId = 9,
                                                .expiresAt = 0});
  CHECK(fenceListenReplay.duplicate);
  CHECK(fenceListenReplay.text == transcribed);
  {
    std::lock_guard lock(captureMutex);
    CHECK(blockingCaptures == 1);
  }
  audio_capture::setCaptureFunctionForTest({});

  std::atomic<int> newClaims{0};
  std::atomic<int> inFlightClaims{0};
  std::barrier claimGate(3);
  const auto raceClaim = [&]() {
    const auto outcome =
        drogon::sync_wait(commands.claim({.commandId = "cmd-race-claim",
                                          .kind = "announce",
                                          .cameraId = 1,
                                          .fingerprint = {},
                                          .at = 200,
                                          .leaseSeconds = 120}));
    if (outcome.kind == ActionClaimKind::New)
      ++newClaims;
    else if (outcome.kind == ActionClaimKind::InFlight)
      ++inFlightClaims;
  };
  std::thread claimA([&]() {
    claimGate.arrive_and_wait();
    raceClaim();
  });
  std::thread claimB([&]() {
    claimGate.arrive_and_wait();
    raceClaim();
  });
  claimGate.arrive_and_wait();
  claimA.join();
  claimB.join();
  CHECK(newClaims == 1);
  CHECK(inFlightClaims == 1);

  CHECK(drogon::sync_wait(commands.settle({.commandId = "cmd-race-claim",
                                           .status = "retryable_failed",
                                           .detail = "boom",
                                           .response = {},
                                           .at = 201})));
  const auto rearmed =
      drogon::sync_wait(commands.claim({.commandId = "cmd-race-claim",
                                        .kind = "announce",
                                        .cameraId = 1,
                                        .fingerprint = {},
                                        .at = 202,
                                        .leaseSeconds = 120}));
  CHECK(rearmed.kind == ActionClaimKind::Retry);

  const auto stale =
      drogon::sync_wait(commands.claim({.commandId = "cmd-race-claim",
                                        .kind = "announce",
                                        .cameraId = 1,
                                        .fingerprint = {},
                                        .at = 1000,
                                        .leaseSeconds = 120}));
  CHECK(stale.kind == ActionClaimKind::InFlight);
  CHECK(stale.detail == "expired");

  CHECK_FALSE(drogon::sync_wait(commands.settle({.commandId = "cmd-race-claim",
                                                 .generation = 0,
                                                 .status = "succeeded",
                                                 .detail = "stale",
                                                 .response = {},
                                                 .at = 1001})));
  CHECK(drogon::sync_wait(
      commands.settle({.commandId = "cmd-race-claim",
                       .generation = rearmed.generation,
                       .status = "succeeded",
                       .detail = "done",
                       .response = "{\"accepted\":true,\"detail\":\"done\"}",
                       .at = 1002})));
  const auto settled =
      drogon::sync_wait(commands.claim({.commandId = "cmd-race-claim",
                                        .kind = "announce",
                                        .cameraId = 1,
                                        .fingerprint = {},
                                        .at = 1003,
                                        .leaseSeconds = 120}));
  CHECK(settled.kind == ActionClaimKind::Completed);
  CHECK(settled.response.find("accepted") != std::string::npos);

  CHECK(drogon::sync_wait(commands.claim({.commandId = "cmd-crashed",
                                          .kind = "alarm",
                                          .cameraId = 1,
                                          .fingerprint = {},
                                          .at = 2000,
                                          .leaseSeconds = 120}))
            .kind == ActionClaimKind::New);
  CHECK(drogon::sync_wait(commands.reconcileExpired(2100, 60)) == 1);
  const auto recovered =
      drogon::sync_wait(commands.claim({.commandId = "cmd-crashed",
                                        .kind = "alarm",
                                        .cameraId = 1,
                                        .fingerprint = {},
                                        .at = 2101,
                                        .leaseSeconds = 120}));
  CHECK(recovered.kind == ActionClaimKind::Indeterminate);
  const auto indeterminate = client.alarm({.cameraId = 1,
                                           .seconds = 2,
                                           .commandId = "cmd-crashed",
                                           .encounterId = 9,
                                           .expiresAt = 0});
  CHECK_FALSE(indeterminate.succeeded());
  CHECK(indeterminate.outcome ==
        argus::camera::v1::CommandOutcome::INDETERMINATE);
  CHECK_FALSE(indeterminate.duplicate);

  const int speakCallsBefore = actuator->speakCalls.load();
  int indeterminateCaptures = 0;
  audio_capture::setCaptureFunctionForTest(
      [&indeterminateCaptures](const AudioCaptureInput&) {
        ++indeterminateCaptures;
        AudioCaptureResult result;
        result.ok = true;
        return result;
      });
  for (const char* kind : {"announce", "alarm", "siren", "listen"}) {
    const std::string kindName(kind);
    const std::string commandId = "cmd-indeterminate-" + kindName;
    REQUIRE(drogon::sync_wait(commands.claim({.commandId = commandId,
                                              .kind = kindName,
                                              .cameraId = 1,
                                              .fingerprint = {},
                                              .at = 3000,
                                              .leaseSeconds = 60}))
                .kind == ActionClaimKind::New);
    REQUIRE(drogon::sync_wait(commands.reconcileExpired(4000, 60)) == 1);
    if (kindName == "announce") {
      const auto ack = client.announce({.cameraId = 1,
                                        .text = "Hola",
                                        .lang = "es",
                                        .commandId = commandId,
                                        .encounterId = 9,
                                        .expiresAt = 0});
      CHECK_FALSE(ack.succeeded());
      CHECK(ack.outcome == argus::camera::v1::CommandOutcome::INDETERMINATE);
    }
    else if (kindName == "alarm") {
      const auto ack = client.alarm({.cameraId = 1,
                                     .seconds = 2,
                                     .commandId = commandId,
                                     .encounterId = 9,
                                     .expiresAt = 0});
      CHECK_FALSE(ack.succeeded());
      CHECK(ack.outcome == argus::camera::v1::CommandOutcome::INDETERMINATE);
    }
    else if (kindName == "siren") {
      const auto ack = client.setSiren({.cameraId = 1,
                                        .enabled = true,
                                        .commandId = commandId,
                                        .encounterId = 9,
                                        .expiresAt = 0,
                                        .leaseSeconds = 20});
      CHECK_FALSE(ack.succeeded());
      CHECK(ack.outcome == argus::camera::v1::CommandOutcome::INDETERMINATE);
    }
    else {
      const auto ack = client.listen({.cameraId = 1,
                                      .seconds = 1,
                                      .lang = "es",
                                      .commandId = commandId,
                                      .encounterId = 9,
                                      .expiresAt = 0});
      CHECK(ack.indeterminate());
    }
  }
  CHECK(actuator->speakCalls.load() == speakCallsBefore);
  CHECK(indeterminateCaptures == 0);
  audio_capture::setCaptureFunctionForTest({});

  {
    // A settle fenced by a concurrent state change must not report success.
    auto fenced = std::make_shared<StubActuator>();
    fenced->blockSpeak = true;
    fenced->speakReleased = false;
    CameraDriverTestAccess::install(1, fenced);
    CameraCommandResult settleAck;
    std::thread settleThread([&]() {
      settleAck = client.announce({.cameraId = 1,
                                   .text = "Fence settle",
                                   .lang = "es",
                                   .commandId = "cmd-settle-fence",
                                   .encounterId = 9,
                                   .expiresAt = 0});
    });
    while (fenced->speakCalls.load() == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const int64_t future = static_cast<int64_t>(std::time(nullptr)) + 100;
    CHECK(drogon::sync_wait(commands.reconcileExpired(future, 0)) == 1);
    fenced->releaseSpeak();
    settleThread.join();
    CHECK_FALSE(settleAck.succeeded());
    CHECK(settleAck.indeterminate());
    CHECK(fenced->speakCalls.load() == 1);
    const auto fencedRow = DbService::client()->execSqlSync(
        "SELECT status FROM action_command WHERE command_id = ?",
        std::string("cmd-settle-fence"));
    REQUIRE(fencedRow.size() == 1);
    CHECK(fencedRow.front()["status"].as<std::string>() == "indeterminate");
  }

  {
    // Alarm fence: blocked driver, concurrent invalidation, authoritative
    // outcome returned and never overwritten.
    auto blocked = std::make_shared<StubActuator>();
    blocked->blockSpeak = true;
    CameraDriverTestAccess::install(1, blocked);
    CameraCommandResult ack;
    std::thread thread([&]() {
      ack = client.alarm({.cameraId = 1,
                          .seconds = 1,
                          .commandId = "cmd-alarm-fence",
                          .encounterId = 9,
                          .expiresAt = 0});
    });
    while (blocked->speakCalls.load() == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const int64_t alarmFuture = static_cast<int64_t>(std::time(nullptr)) + 100;
    CHECK(drogon::sync_wait(commands.reconcileExpired(alarmFuture, 0)) == 1);
    blocked->releaseSpeak();
    thread.join();
    CHECK(ack.indeterminate());
    CHECK(blocked->speakCalls.load() == 1);
  }

  {
    // Siren fence: the lease is persisted, hardware is invoked, and the fenced
    // settle returns the authoritative indeterminate outcome.
    auto blocked = std::make_shared<StubActuator>();
    blocked->blockSettings = true;
    CameraDriverTestAccess::install(1, blocked);
    CameraCommandResult ack;
    std::thread thread([&]() {
      ack = client.setSiren({.cameraId = 1,
                             .enabled = true,
                             .commandId = "cmd-siren-fence",
                             .encounterId = 9,
                             .expiresAt = 0,
                             .leaseSeconds = 20});
    });
    while (!blocked->settingsCalled)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const int64_t sirenFuture = static_cast<int64_t>(std::time(nullptr)) + 100;
    CHECK(drogon::sync_wait(commands.reconcileExpired(sirenFuture, 0)) == 1);
    blocked->releaseSettings();
    thread.join();
    CHECK(ack.indeterminate());
  }

  {
    // Listen fence: blocked capture, concurrent invalidation, no capture
    // reported as a normal answer.
    std::mutex captureMutex;
    std::condition_variable captureCv;
    bool captureReleased = false;
    bool captureStarted = false;
    audio_capture::setCaptureFunctionForTest([&](const AudioCaptureInput&) {
      {
        std::lock_guard lock(captureMutex);
        captureStarted = true;
      }
      {
        std::unique_lock lock(captureMutex);
        captureCv.wait(lock, [&captureReleased]() { return captureReleased; });
      }
      AudioCaptureResult result;
      result.ok = true;
      result.speechDetected = true;
      result.samples.assign(16000, 0);
      return result;
    });
    CameraDriverTestAccess::install(1, std::make_shared<StubActuator>());
    CameraCommandResult ack;
    std::thread thread([&]() {
      ack = client.listen({.cameraId = 1,
                           .seconds = 1,
                           .lang = "es",
                           .commandId = "cmd-listen-fence-race",
                           .encounterId = 9,
                           .expiresAt = 0});
    });
    while (true) {
      std::lock_guard lock(captureMutex);
      if (captureStarted)
        break;
    }
    const int64_t listenFuture = static_cast<int64_t>(std::time(nullptr)) + 100;
    CHECK(drogon::sync_wait(commands.reconcileExpired(listenFuture, 0)) == 1);
    {
      std::lock_guard lock(captureMutex);
      captureReleased = true;
    }
    captureCv.notify_all();
    thread.join();
    CHECK(ack.indeterminate());
    CHECK_FALSE(ack.captured);
    audio_capture::setCaptureFunctionForTest({});
  }

  const auto statusOf = [](const std::string& commandId) {
    const auto rows = DbService::client()->execSqlSync(
        "SELECT status FROM action_command WHERE command_id = ?", commandId);
    return rows.empty() ? std::string{}
                        : rows.front()["status"].as<std::string>();
  };

  {
    // TTS is unavailable before any hardware call: proven retryable.
    ConfigService::setRuntimeString("tts.remote_url", "http://127.0.0.1:1");
    const int speakBefore = actuator->speakCalls.load();
    const auto ttsDown = client.announce({.cameraId = 1,
                                          .text = "Hola",
                                          .lang = "es",
                                          .commandId = "cmd-tts-down",
                                          .encounterId = 9,
                                          .expiresAt = 0});
    CHECK(ttsDown.retryable());
    CHECK_FALSE(ttsDown.succeeded());
    CHECK(actuator->speakCalls.load() == speakBefore);
    CHECK(statusOf("cmd-tts-down") == "retryable_failed");
    ConfigService::setRuntimeString("tts.remote_url",
                                    "http://127.0.0.1:" +
                                        std::to_string(tts.port()));
  }

  {
    // The driver fails after the side effect marker: indeterminate.
    auto exploding = std::make_shared<StubActuator>();
    exploding->throwOnSpeak = true;
    CameraDriverTestAccess::install(1, exploding);
    const auto ack = client.announce({.cameraId = 1,
                                      .text = "Hola",
                                      .lang = "es",
                                      .commandId = "cmd-announce-throw",
                                      .encounterId = 9,
                                      .expiresAt = 0});
    CHECK(ack.indeterminate());
    CHECK(exploding->speakCalls.load() == 1);
    CHECK(statusOf("cmd-announce-throw") == "indeterminate");
  }

  {
    auto exploding = std::make_shared<StubActuator>();
    exploding->throwOnSpeak = true;
    CameraDriverTestAccess::install(1, exploding);
    const auto ack = client.alarm({.cameraId = 1,
                                   .seconds = 1,
                                   .commandId = "cmd-alarm-throw",
                                   .encounterId = 9,
                                   .expiresAt = 0});
    CHECK(ack.indeterminate());
    CHECK(exploding->speakCalls.load() == 1);
    CHECK(statusOf("cmd-alarm-throw") == "indeterminate");
  }

  {
    // The lease must be durable before hardware; a throwing driver stays
    // indeterminate and the lease remains for the sweeper.
    auto exploding = std::make_shared<StubActuator>();
    exploding->throwOnSettings = true;
    CameraDriverTestAccess::install(1, exploding);
    const auto ack = client.setSiren({.cameraId = 1,
                                      .enabled = true,
                                      .commandId = "cmd-siren-throw",
                                      .encounterId = 9,
                                      .expiresAt = 0,
                                      .leaseSeconds = 20});
    CHECK(ack.indeterminate());
    CHECK(exploding->settingsCalled);
    CHECK(statusOf("cmd-siren-throw") == "indeterminate");
    const auto leases = DbService::client()->execSqlSync(
        "SELECT COUNT(*) AS total FROM siren_lease WHERE camera_id = 1");
    CHECK(leases.front()["total"].as<int>() == 1);
  }

  {
    AudioCaptureResult dummy;
    audio_capture::setCaptureFunctionForTest(
        [](const AudioCaptureInput&) -> AudioCaptureResult {
          throw std::runtime_error("capture exploded");
        });
    auto plain = std::make_shared<StubActuator>();
    CameraDriverTestAccess::install(1, plain);
    const auto ack = client.listen({.cameraId = 1,
                                    .seconds = 1,
                                    .lang = "es",
                                    .commandId = "cmd-listen-throw",
                                    .encounterId = 9,
                                    .expiresAt = 0});
    CHECK(ack.indeterminate());
    CHECK(statusOf("cmd-listen-throw") == "indeterminate");
    audio_capture::setCaptureFunctionForTest({});
  }

  writeConfig(false);
  ConfigService::load(kConfig);
  CHECK(client
            .announce({.cameraId = 1,
                       .text = "Hola",
                       .lang = "es",
                       .commandId = "cmd-disabled",
                       .encounterId = 9,
                       .expiresAt = 0})
            .rejected());

  server->Shutdown();
  drogon::app().quit();
  runner.join();
  std::remove(kCameraDb);
  std::remove((std::string(kCameraDb) + "-wal").c_str());
  std::remove((std::string(kCameraDb) + "-shm").c_str());
  std::remove(kConfig);
}

TEST_CASE("camera command retryable classification is precise")
{
  using Outcome = argus::camera::v1::CommandOutcome;
  const auto make = [](grpc::StatusCode code, Outcome outcome) {
    CameraCommandResult result;
    result.status = grpc::Status(code, "");
    result.outcome = outcome;
    return result;
  };
  CHECK(
      make(grpc::StatusCode::UNAVAILABLE, Outcome::COMMAND_OUTCOME_UNSPECIFIED)
          .retryable());
  CHECK(make(grpc::StatusCode::DEADLINE_EXCEEDED,
             Outcome::COMMAND_OUTCOME_UNSPECIFIED)
            .retryable());
  CHECK_FALSE(make(grpc::StatusCode::UNAUTHENTICATED,
                   Outcome::COMMAND_OUTCOME_UNSPECIFIED)
                  .retryable());
  CHECK_FALSE(make(grpc::StatusCode::PERMISSION_DENIED,
                   Outcome::COMMAND_OUTCOME_UNSPECIFIED)
                  .retryable());
  CHECK_FALSE(make(grpc::StatusCode::INVALID_ARGUMENT,
                   Outcome::COMMAND_OUTCOME_UNSPECIFIED)
                  .retryable());
  CHECK_FALSE(make(grpc::StatusCode::FAILED_PRECONDITION,
                   Outcome::COMMAND_OUTCOME_UNSPECIFIED)
                  .retryable());
  CHECK_FALSE(make(grpc::StatusCode::ALREADY_EXISTS,
                   Outcome::COMMAND_OUTCOME_UNSPECIFIED)
                  .retryable());
  CHECK_FALSE(
      make(grpc::StatusCode::INTERNAL, Outcome::COMMAND_OUTCOME_UNSPECIFIED)
          .retryable());
  CHECK(make(grpc::StatusCode::OK, Outcome::RETRYABLE_FAILED).retryable());
  CHECK_FALSE(make(grpc::StatusCode::OK, Outcome::SUCCEEDED).retryable());
  CHECK_FALSE(make(grpc::StatusCode::OK, Outcome::INDETERMINATE).retryable());
  CHECK_FALSE(make(grpc::StatusCode::OK, Outcome::REJECTED).retryable());
  CHECK_FALSE(make(grpc::StatusCode::OK, Outcome::CONFLICT).retryable());
  CHECK(make(grpc::StatusCode::OK, Outcome::IN_FLIGHT).inFlight());
}
