#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/camera-driver/camera-scene-log.hxx>
#include <shared/services/privacy-state/privacy-state.hxx>
#include <shared/services/stream/snapshot-store.hxx>
#include <sync/camera-change-sink.hxx>

#include <cstdint>
#include <json/value.h>
#include <memory>
#include <string>
#include <utility>
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
}

TEST_CASE("the boot seed turns persisted states into the live scene")
{
  PrivacyStateService::instance().seed(
      {cameraOf(11, CameraPrivacyState::On, "[]"),
       cameraOf(12, CameraPrivacyState::Off, "[]"),
       cameraOf(13, CameraPrivacyState::Unknown, "[]")});
  CHECK(PrivacyStateService::instance().masked(11));
  CHECK_FALSE(PrivacyStateService::instance().masked(12));
  CHECK_FALSE(PrivacyStateService::instance().masked(13));
  CHECK(PrivacyStateService::instance().stateOf(11) == CameraPrivacyState::On);
  CHECK(PrivacyStateService::instance().stateOf(12) == CameraPrivacyState::Off);
}

TEST_CASE("a refresh records a transition once, audits it and announces it")
{
  FakeChangeSink sink;
  camera_change::setSink(&sink);
  PrivacyStateService::instance().forget(21);
  const CameraSchema camera = cameraOf(21, CameraPrivacyState::Unknown, "[]");

  const auto on = [&]() {
    return PrivacyStateService::instance()
        .refresh({.camera = camera, .driverStatus = statusWith(true), .readable = true});
  };
  const auto applied = drogon::sync_wait(on());
  CHECK(applied.changed);
  CHECK(applied.after == CameraPrivacyState::On);
  CHECK(PrivacyStateService::instance().masked(21));
  CHECK(sink.audits.size() == 1);
  CHECK(sink.audits.front().before.find("\"on\"") == std::string::npos);
  CHECK(sink.audits.front().after.find("\"on\"") != std::string::npos);
  CHECK(sink.resources.size() == 2);
  CHECK(sink.resources.front().scope == ResourceScope::Status);
  CHECK(sink.resources.back().scope == ResourceScope::Overview);

  const auto again = drogon::sync_wait(on());
  CHECK_FALSE(again.changed);
  CHECK(sink.audits.size() == 1);
  CHECK(sink.resources.size() == 2);

  const auto off = drogon::sync_wait(
      PrivacyStateService::instance().refresh(
          {.camera = camera, .driverStatus = statusWith(false), .readable = true}));
  CHECK(off.changed);
  CHECK(off.after == CameraPrivacyState::Off);
  CHECK_FALSE(PrivacyStateService::instance().masked(21));
  CHECK(sink.audits.size() == 2);
  CHECK(sink.audits.back().before.find("\"on\"") != std::string::npos);
  CHECK(sink.audits.back().after.find("\"off\"") != std::string::npos);
  camera_change::setSink(nullptr);
}

TEST_CASE("an unreadable or unreturned state is unknown, never off")
{
  camera_change::setSink(nullptr);
  PrivacyStateService::instance().forget(31);
  const CameraSchema camera = cameraOf(31, CameraPrivacyState::Unknown, "[]");

  const auto unreadable = drogon::sync_wait(
      PrivacyStateService::instance().refresh(
          {.camera = camera, .driverStatus = Json::Value(), .readable = false}));
  CHECK_FALSE(unreadable.changed);
  CHECK(unreadable.after == CameraPrivacyState::Off);
  CHECK_FALSE(PrivacyStateService::instance().masked(31));

  const auto noBool = drogon::sync_wait(
      PrivacyStateService::instance().refresh(
          {.camera = camera, .driverStatus = Json::Value(Json::objectValue),
           .readable = true}));
  CHECK(noBool.after == CameraPrivacyState::Unknown);
  CHECK_FALSE(PrivacyStateService::instance().masked(31));

  PrivacyStateService::instance().seed({cameraOf(32, CameraPrivacyState::On, "[]")});
  const CameraSchema maskedCamera = cameraOf(32, CameraPrivacyState::On, "[]");
  const auto error = drogon::sync_wait(
      PrivacyStateService::instance().refresh(
          {.camera = maskedCamera, .driverStatus = Json::Value(), .readable = false}));
  CHECK_FALSE(error.changed);
  CHECK(PrivacyStateService::instance().masked(32));
}

TEST_CASE("the mask clears the stored frame and keeps the state on the camera")
{
  SnapshotStore::instance().putFrame(
      {.cameraId = 41, .jpeg = "frame-bytes", .atMs = 1000});
  CHECK(SnapshotStore::instance().frame(41).has_value());

  PrivacyStateService::instance().seed({cameraOf(41, CameraPrivacyState::Off, "[]")});
  const CameraSchema camera = cameraOf(41, CameraPrivacyState::Off, "[]");
  const auto masked = drogon::sync_wait(
      PrivacyStateService::instance().refresh(
          {.camera = camera, .driverStatus = statusWith(true), .readable = true}));
  CHECK(masked.after == CameraPrivacyState::On);
  CHECK_FALSE(SnapshotStore::instance().frame(41).has_value());
}

TEST_CASE("only a camera whose capabilities claim privacy is polled")
{
  CHECK(PrivacyStateService::privacyCapable(
      cameraOf(51, CameraPrivacyState::Unknown, "[\"ptz\",\"privacy\",\"talk\"]")));
  CHECK_FALSE(PrivacyStateService::privacyCapable(
      cameraOf(52, CameraPrivacyState::Unknown, "[\"ptz\",\"talk\"]")));
  CHECK_FALSE(PrivacyStateService::privacyCapable(
      cameraOf(53, CameraPrivacyState::Unknown, "[]")));
  CHECK_FALSE(PrivacyStateService::privacyCapable(
      cameraOf(54, CameraPrivacyState::Unknown, "not-json")));
}
