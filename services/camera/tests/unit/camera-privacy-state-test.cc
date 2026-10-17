#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <camera/camera-privacy-state.hxx>
#include <drogon/drogon.h>
#include <shared/schemas/camera/camera-schema.hxx>
#include <shared/services/privacy-state/privacy-state.hxx>
#include <shared/services/stream/snapshot-store.hxx>
#include <sqlite/db-service.hxx>
#include <sync/camera-change-sink.hxx>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <json/value.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct AuditCall
{
  int64_t recordId{0};
  std::string before;
  std::string after;
};

struct ResourceCall
{
  ResourceScope scope{ResourceScope::Status};
  std::vector<int64_t> ids;
};

class FakeChangeSink : public CameraChangeSink
{
public:
  drogon::Task<void> emitModule(const ModuleEmitInput&) const override
  {
    co_return;
  }
  drogon::Task<void> publishAudit(const ModuleAuditInput& input) const override
  {
    audits.push_back({.recordId = input.recordId,
                      .before = input.before.toStyledString(),
                      .after = input.after.toStyledString()});
    co_return;
  }
  drogon::Task<void> publishResource(
      const CameraResourceChange& input) const override
  {
    resources.push_back({.scope = input.scope, .ids = input.ids});
    co_return;
  }
  drogon::Task<void> publishAction(const ActionPublishInput&) const override
  {
    co_return;
  }

  mutable std::vector<AuditCall> audits;
  mutable std::vector<ResourceCall> resources;
};

constexpr const char* kPrivacyDb = "camera-privacy-state-test.db";

class PrivacyRig
{
public:
  static PrivacyRig& instance()
  {
    static PrivacyRig rig;
    return rig;
  }

  [[nodiscard]] bool booted() const { return booted_; }

  void resetCameras()
  {
    client_->execSqlSync("DROP TABLE IF EXISTS camera");
    REQUIRE(DbService::runScriptFile(ARGUS_CAMERA_SCHEMA_PATH, client_));
  }

  void dropCameras() { client_->execSqlSync("DROP TABLE IF EXISTS camera"); }

  void addCamera(int64_t id, const std::string& state)
  {
    client_->execSqlSync(
        "INSERT INTO camera (id, name, ip, privacy_state) VALUES (?, 'Cam', "
        "'192.168.1.30', ?)",
        id, state);
  }

  [[nodiscard]] std::string stateOfRow(int64_t id) const
  {
    const auto rows = client_->execSqlSync(
        "SELECT privacy_state FROM camera WHERE id = ?", id);
    return rows.empty() ? std::string()
                        : rows.front()["privacy_state"].as<std::string>();
  }

private:
  PrivacyRig()
  {
    std::remove(kPrivacyDb);
    std::remove((std::string(kPrivacyDb) + "-wal").c_str());
    std::remove((std::string(kPrivacyDb) + "-shm").c_str());
    drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                        .filename = kPrivacyDb,
                                                        .name = "default",
                                                        .timeout = -1});
    std::thread runner([] { drogon::app().run(); });
    runner.detach();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!drogon::app().isRunning() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    booted_ = drogon::app().isRunning();
    client_ = DbService::cameraClient();
  }

  bool booted_{false};
  drogon::orm::DbClientPtr client_;
};

CameraSchema cameraOf(int64_t id, CameraPrivacyState state,
                      const std::string& capabilities)
{
  CameraSchema camera;
  camera.id = id;
  camera.name = "Cam";
  camera.capabilities = capabilities;
  camera.privacyState = state;
  return camera;
}

Json::Value statusWith(bool enabled)
{
  Json::Value status(Json::objectValue);
  status["privacyEnabled"] = enabled;
  return status;
}

Json::Value statusWithout()
{
  return Json::Value(Json::objectValue);
}

CameraPrivacyState readOf(int64_t cameraId)
{
  return PrivacyStateService::instance().stateOf(cameraId);
}

PrivacyTransition read(int64_t cameraId, const Json::Value& status,
                       CameraPrivacyState state)
{
  return drogon::sync_wait(PrivacyStateService::instance().refresh(
      {.camera = cameraOf(cameraId, state, "[]"),
       .driverStatus = status,
       .readable = true}));
}
}

TEST_CASE("the seed keeps the tri-state, so a camera never read is unknown, not off")
{
  auto& service = PrivacyStateService::instance();
  service.seed({cameraOf(11, CameraPrivacyState::On, "[]"),
                cameraOf(12, CameraPrivacyState::Off, "[]"),
                cameraOf(13, CameraPrivacyState::Unknown, "[]")});
  CHECK(readOf(11) == CameraPrivacyState::On);
  CHECK(readOf(12) == CameraPrivacyState::Off);
  CHECK(readOf(13) == CameraPrivacyState::Unknown);
  CHECK(service.masked(11));
  CHECK_FALSE(service.masked(12));
  CHECK_FALSE(service.masked(13));
  CHECK(service.maskedOf(cameraOf(11, CameraPrivacyState::Unknown, "[]")));
  CHECK_FALSE(service.maskedOf(cameraOf(13, CameraPrivacyState::Unknown, "[]")));
  CHECK(readOf(14) == CameraPrivacyState::Unknown);
  CHECK_FALSE(service.masked(14));

  service.forget(11);
  CHECK(readOf(11) == CameraPrivacyState::Unknown);
  CHECK_FALSE(service.masked(11));
}

TEST_CASE("an unreadable read keeps the last state and publishes nothing")
{
  FakeChangeSink sink;
  camera_change::setSink(&sink);
  auto& service = PrivacyStateService::instance();
  service.seed({cameraOf(21, CameraPrivacyState::On, "[]"),
                cameraOf(22, CameraPrivacyState::Unknown, "[]")});

  const auto masked = drogon::sync_wait(service.refresh(
      {.camera = cameraOf(21, CameraPrivacyState::On, "[]"),
       .driverStatus = Json::Value(),
       .readable = false}));
  CHECK_FALSE(masked.changed);
  CHECK(masked.before == CameraPrivacyState::On);
  CHECK(masked.after == CameraPrivacyState::On);
  CHECK(service.masked(21));

  const auto never = drogon::sync_wait(service.refresh(
      {.camera = cameraOf(22, CameraPrivacyState::Unknown, "[]"),
       .driverStatus = Json::Value(),
       .readable = false}));
  CHECK_FALSE(never.changed);
  CHECK(never.after == CameraPrivacyState::Unknown);
  CHECK_FALSE(service.masked(22));

  CHECK(sink.audits.empty());
  CHECK(sink.resources.empty());
  camera_change::setSink(nullptr);
}

TEST_CASE("a mask transition audits once, announces twice and persists the row")
{
  REQUIRE(PrivacyRig::instance().booted());
  auto& rig = PrivacyRig::instance();
  FakeChangeSink sink;
  camera_change::setSink(&sink);
  rig.resetCameras();
  rig.addCamera(31, "off");
  auto& service = PrivacyStateService::instance();
  service.forget(31);
  service.seed({cameraOf(31, CameraPrivacyState::Off, "[]")});

  const auto on = read(31, statusWith(true), CameraPrivacyState::Off);
  CHECK(on.changed);
  CHECK(on.before == CameraPrivacyState::Off);
  CHECK(on.after == CameraPrivacyState::On);
  CHECK(service.masked(31));
  CHECK(rig.stateOfRow(31) == "on");
  REQUIRE(sink.audits.size() == 1);
  CHECK(sink.audits.front().before.find("\"on\"") == std::string::npos);
  CHECK(sink.audits.front().after.find("\"on\"") != std::string::npos);
  REQUIRE(sink.resources.size() == 2);
  CHECK(sink.resources.front().scope == ResourceScope::Status);
  CHECK(sink.resources.back().scope == ResourceScope::Overview);

  const auto again = read(31, statusWith(true), CameraPrivacyState::On);
  CHECK_FALSE(again.changed);
  CHECK(sink.audits.size() == 1);
  CHECK(sink.resources.size() == 2);

  const auto off = read(31, statusWith(false), CameraPrivacyState::On);
  CHECK(off.changed);
  CHECK(off.after == CameraPrivacyState::Off);
  CHECK_FALSE(service.masked(31));
  CHECK(rig.stateOfRow(31) == "off");
  CHECK(sink.audits.size() == 2);
  CHECK(sink.audits.back().before.find("\"on\"") != std::string::npos);
  CHECK(sink.audits.back().after.find("\"off\"") != std::string::npos);
  CHECK(sink.resources.size() == 4);
  camera_change::setSink(nullptr);
}

TEST_CASE("a reply with no mask bool converges the row to unknown, once")
{
  REQUIRE(PrivacyRig::instance().booted());
  auto& rig = PrivacyRig::instance();
  FakeChangeSink sink;
  camera_change::setSink(&sink);
  rig.resetCameras();
  rig.addCamera(41, "off");
  auto& service = PrivacyStateService::instance();
  service.forget(41);
  service.seed({cameraOf(41, CameraPrivacyState::Off, "[]")});

  const auto quiet = read(41, statusWithout(), CameraPrivacyState::Off);
  CHECK(quiet.changed);
  CHECK(quiet.after == CameraPrivacyState::Unknown);
  CHECK_FALSE(service.masked(41));
  CHECK(rig.stateOfRow(41) == "unknown");
  CHECK(sink.audits.size() == 1);
  CHECK(sink.resources.size() == 2);

  for (int tick = 0; tick < 3; ++tick) {
    const auto silent = read(41, statusWithout(), CameraPrivacyState::Unknown);
    CHECK_FALSE(silent.changed);
  }
  CHECK(sink.audits.size() == 1);
  CHECK(sink.resources.size() == 2);
  camera_change::setSink(nullptr);
}

TEST_CASE("a migrated unknown converges to off on the first good read, once")
{
  REQUIRE(PrivacyRig::instance().booted());
  auto& rig = PrivacyRig::instance();
  FakeChangeSink sink;
  camera_change::setSink(&sink);
  rig.resetCameras();
  rig.addCamera(51, "unknown");
  auto& service = PrivacyStateService::instance();
  service.forget(51);
  service.seed({cameraOf(51, CameraPrivacyState::Unknown, "[]")});

  const auto converged = read(51, statusWith(false), CameraPrivacyState::Unknown);
  CHECK(converged.changed);
  CHECK(converged.before == CameraPrivacyState::Unknown);
  CHECK(converged.after == CameraPrivacyState::Off);
  CHECK(readOf(51) == CameraPrivacyState::Off);
  CHECK(rig.stateOfRow(51) == "off");
  CHECK(sink.audits.size() == 1);

  const auto again = read(51, statusWith(false), CameraPrivacyState::Off);
  CHECK_FALSE(again.changed);
  CHECK(sink.audits.size() == 1);
  CHECK(sink.resources.size() == 2);
  camera_change::setSink(nullptr);
}

TEST_CASE("two refreshes of one camera in flight give exactly one audit")
{
  REQUIRE(PrivacyRig::instance().booted());
  auto& rig = PrivacyRig::instance();
  FakeChangeSink sink;
  camera_change::setSink(&sink);
  rig.resetCameras();
  rig.addCamera(61, "off");
  auto& service = PrivacyStateService::instance();
  service.forget(61);
  service.seed({cameraOf(61, CameraPrivacyState::Off, "[]")});

  const auto status = statusWith(true);
  const auto once = [&status] {
    const auto transition = read(61, status, CameraPrivacyState::Off);
    static_cast<void>(transition);
  };
  std::thread first(once);
  std::thread second(once);
  first.join();
  second.join();

  CHECK(sink.audits.size() == 1);
  CHECK(sink.resources.size() == 2);
  CHECK(service.masked(61));
  CHECK(rig.stateOfRow(61) == "on");
  camera_change::setSink(nullptr);
}

TEST_CASE("a write that fails sends no audit, obeys the mask and retries")
{
  REQUIRE(PrivacyRig::instance().booted());
  auto& rig = PrivacyRig::instance();
  FakeChangeSink sink;
  camera_change::setSink(&sink);
  rig.resetCameras();
  rig.addCamera(71, "off");
  auto& service = PrivacyStateService::instance();
  service.forget(71);
  service.seed({cameraOf(71, CameraPrivacyState::Off, "[]")});

  rig.dropCameras();
  const auto masked = read(71, statusWith(true), CameraPrivacyState::Off);
  CHECK(masked.changed);
  CHECK(masked.after == CameraPrivacyState::On);
  CHECK(service.masked(71));
  CHECK(sink.audits.empty());
  CHECK(sink.resources.empty());

  rig.resetCameras();
  rig.addCamera(71, "off");
  const auto retried = read(71, statusWith(true), CameraPrivacyState::On);
  CHECK_FALSE(retried.changed);
  CHECK(rig.stateOfRow(71) == "on");
  CHECK(sink.audits.empty());
  CHECK(sink.resources.empty());
  camera_change::setSink(nullptr);
}

TEST_CASE("a failed unmask is rolled back and honoured on the next read")
{
  REQUIRE(PrivacyRig::instance().booted());
  auto& rig = PrivacyRig::instance();
  FakeChangeSink sink;
  camera_change::setSink(&sink);
  rig.resetCameras();
  rig.addCamera(81, "on");
  auto& service = PrivacyStateService::instance();
  service.forget(81);
  service.seed({cameraOf(81, CameraPrivacyState::On, "[]")});

  rig.dropCameras();
  const auto refused = read(81, statusWith(false), CameraPrivacyState::On);
  CHECK_FALSE(refused.changed);
  CHECK(refused.after == CameraPrivacyState::On);
  CHECK(service.masked(81));
  CHECK(sink.audits.empty());
  CHECK(sink.resources.empty());

  rig.resetCameras();
  rig.addCamera(81, "on");
  const auto unmasked = read(81, statusWith(false), CameraPrivacyState::On);
  CHECK(unmasked.changed);
  CHECK(unmasked.after == CameraPrivacyState::Off);
  CHECK_FALSE(service.masked(81));
  CHECK(rig.stateOfRow(81) == "off");
  CHECK(sink.audits.size() == 1);
  CHECK(sink.resources.size() == 2);
  camera_change::setSink(nullptr);
}

TEST_CASE("the mask clears the stored frame and keeps the state on the camera")
{
  SnapshotStore::instance().putFrame(
      {.cameraId = 91, .jpeg = "frame-bytes", .atMs = 1000});
  CHECK(SnapshotStore::instance().frame(91).has_value());

  auto& service = PrivacyStateService::instance();
  service.seed({cameraOf(91, CameraPrivacyState::Off, "[]")});
  const auto masked = read(91, statusWith(true), CameraPrivacyState::Off);
  CHECK(masked.changed);
  CHECK(masked.after == CameraPrivacyState::On);
  CHECK_FALSE(SnapshotStore::instance().frame(91).has_value());
  service.forget(91);
}

TEST_CASE("only a camera whose capabilities claim privacy is polled")
{
  CHECK(PrivacyStateService::privacyCapable(
      cameraOf(94, CameraPrivacyState::Unknown, "[\"ptz\",\"privacy\",\"talk\"]")));
  CHECK_FALSE(PrivacyStateService::privacyCapable(
      cameraOf(95, CameraPrivacyState::Unknown, "[\"ptz\",\"talk\"]")));
  CHECK_FALSE(PrivacyStateService::privacyCapable(
      cameraOf(96, CameraPrivacyState::Unknown, "[]")));
  CHECK_FALSE(PrivacyStateService::privacyCapable(
      cameraOf(97, CameraPrivacyState::Unknown, "not-json")));
}
