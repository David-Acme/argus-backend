#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "guard-fakes.hxx"

#include <doctest/doctest.h>
#include <errors/response-exception.hxx>
#include <feature/guard/infra/resource-change-outbox.hxx>
#include <feature/safety/services/siren-service.hxx>
#include <nats/nats-bus.hxx>
#include <sync/user-action-event.hxx>
#include <text/json-util.hxx>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using guard_test::GuardBoot;
using guard_test::clearChangeOutbox;
using guard_test::scalar;

namespace
{

GuardBoot& boot()
{
  static GuardBoot shared("guard-siren-test");
  return shared;
}

struct SirenCall
{
  int64_t cameraId{0};
  bool enabled{false};
  std::string commandId;
  int64_t encounterId{0};
  int64_t expiresAt{0};
  int leaseSeconds{0};
};

class RecordingCameraActions final : public CameraActionClient
{
public:
  RecordingCameraActions()
      : CameraActionClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  CameraCommandResult setSiren(const CameraSirenInput& input) const override
  {
    std::scoped_lock lock(mutex_);
    calls_.push_back({.cameraId = input.cameraId,
                      .enabled = input.enabled,
                      .commandId = input.commandId,
                      .encounterId = input.encounterId,
                      .expiresAt = input.expiresAt,
                      .leaseSeconds = input.leaseSeconds});
    if (rejections_ > 0) {
      --rejections_;
      CameraCommandResult rejected;
      rejected.status = grpc::Status::OK;
      rejected.outcome = CameraCommandOutcome::REJECTED;
      rejected.detail = rejection_;
      return rejected;
    }
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::SUCCEEDED;
    result.detail = "new";
    return result;
  }

  CameraCommandResult announce(const CameraAnnounceInput&) const override
  {
    return {};
  }

  CameraCommandResult alarm(const CameraAlarmInput&) const override
  {
    return {};
  }

  std::optional<CameraCrop> personCrop(const CameraPersonCropInput&) const override
  {
    return std::nullopt;
  }

  CameraCommandResult listen(const CameraListenInput&) const override
  {
    return {};
  }

  [[nodiscard]] std::vector<SirenCall> calls() const
  {
    std::scoped_lock lock(mutex_);
    return calls_;
  }

  void rejectNext(const std::string& detail) const
  {
    std::scoped_lock lock(mutex_);
    rejections_ = 1;
    rejection_ = detail;
  }

  void clear() const
  {
    std::scoped_lock lock(mutex_);
    calls_.clear();
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<SirenCall> calls_;
  mutable int rejections_{0};
  mutable std::string rejection_;
};

struct SirenRig
{
  explicit SirenRig(GuardSirenConfig sirenConfig = {.enabled = true,
                                                    .leaseSeconds = 20,
                                                    .expiresMarginSeconds = 60})
      : config(sirenConfig)
  {
    clearChangeOutbox();
    camera.clear();
  }

  GuardSirenConfig config;
  RecordingCameraActions camera;
  std::shared_ptr<NatsBus> bus{std::make_shared<NatsBus>()};
  ResourceChangeOutbox changes{bus};
  int64_t clock{1'700'000'000'000};
  SirenService siren{SirenService::Dependencies{.camera = &camera,
                                                .changes = &changes,
                                                .clock = [this] { return clock; }},
                     config};

  AppSirenResult activate(int64_t cameraId = 3, int64_t actorUserId = 2,
                          bool enabled = true)
  {
    return drogon::sync_wait(siren.apply({.cameraId = cameraId,
                                          .enabled = enabled,
                                          .actorUserId = actorUserId,
                                          .actorIp = "10.0.0.9"}));
  }
};

std::vector<Json::Value> outboxPayloads()
{
  std::vector<Json::Value> payloads;
  const auto rows = DbService::client()->execSqlSync(
      "SELECT payload FROM change_outbox ORDER BY rowid");
  for (const auto& row : rows)
    payloads.push_back(json_util::fromString(row[0].as<std::string>()));
  return payloads;
}

Json::Value payloadOn(const std::string& subject)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT payload FROM change_outbox WHERE subject = ? ORDER BY rowid DESC "
      "LIMIT 1",
      subject);
  return rows.empty() ? Json::Value() : json_util::fromString(rows[0][0].as<std::string>());
}

struct Refusal
{
  int status{0};
  std::string message;
};

Refusal refusalOf(const std::function<void()>& call)
{
  try {
    call();
  }
  catch (const ResponseException& error) {
    return {.status = error.statusCode(), .message = error.what()};
  }
  return {};
}

}

TEST_CASE("the app siren is off until the owner turns it on")
{
  boot();
  SirenRig rig(GuardSirenConfig{.enabled = false,
                                .leaseSeconds = 20,
                                .expiresMarginSeconds = 60});

  CHECK(refusalOf([&] { rig.activate(); }).status == 403);
  CHECK(rig.camera.calls().empty());
  CHECK(outboxPayloads().empty());

  rig.siren.refresh(GuardSirenConfig{.enabled = true,
                                     .leaseSeconds = 20,
                                     .expiresMarginSeconds = 60});
  CHECK(rig.activate(3, 2).enabled);
  CHECK(rig.camera.calls().size() == 1);
}

TEST_CASE("an activation arms the camera siren for the configured lease")
{
  boot();
  SirenRig rig;
  const AppSirenResult result = rig.activate(3, 2);

  CHECK(result.cameraId == 3);
  CHECK(result.enabled);
  CHECK(result.until == 1'700'000'000 + 20);

  REQUIRE(rig.camera.calls().size() == 1);
  const SirenCall call = rig.camera.calls().front();
  CHECK(call.cameraId == 3);
  CHECK(call.enabled);
  CHECK(call.leaseSeconds == 20);
  CHECK(call.leaseSeconds > 0);
  CHECK(call.expiresAt == result.until + 60);
  CHECK(call.encounterId == 0);
  CHECK(call.commandId.starts_with("app-siren:2:"));
}

TEST_CASE("an activation writes one activity audit and one camera notice")
{
  boot();
  SirenRig rig;
  static_cast<void>(rig.activate(7, 4));

  const auto payloads = outboxPayloads();
  REQUIRE(payloads.size() == 2);

  const Json::Value action = payloadOn("argus.guard.v1.user-action");
  REQUIRE(action.isObject());
  CHECK(action["user_id"].asInt64() == 4);
  CHECK(action["record_id"].asInt64() == 7);
  CHECK(action["table_name"].asString() == "camera");
  CHECK(action["action"].asString() == "update");
  CHECK(action["ip_address"].asString() == "10.0.0.9");
  CHECK(action["new_data"]["event"].asString() == "app_siren");
  CHECK(action["new_data"]["enabled"].asBool());
  CHECK(action["new_data"]["cameraId"].asInt64() == 7);
  CHECK(action["new_data"]["at"].asInt64() == 1'700'000'000'000);
  CHECK_FALSE(action.isMember("module"));

  const Json::Value notice = payloadOn("argus.guard.v1.change");
  REQUIRE(notice.isObject());
  CHECK(notice["kind"].asString() == "resource");
  CHECK(notice["domain"].asString() == "camera");
  CHECK(notice["scope"].asString() == "status");
  REQUIRE(notice["ids"].isArray());
  CHECK(notice["ids"][0].asInt64() == 7);
  CHECK_FALSE(notice.isMember("user"));
}

TEST_CASE("a stop releases the lease and audits the stop")
{
  boot();
  SirenRig rig;
  const AppSirenResult result = rig.activate(7, 4, false);

  CHECK_FALSE(result.enabled);
  CHECK(result.until == 0);
  REQUIRE(rig.camera.calls().size() == 1);
  CHECK_FALSE(rig.camera.calls().front().enabled);
  CHECK(rig.camera.calls().front().leaseSeconds == 0);
  CHECK(rig.camera.calls().front().expiresAt == 0);

  const Json::Value action = payloadOn("argus.guard.v1.user-action");
  CHECK_FALSE(action["new_data"]["enabled"].asBool());
  CHECK(action["new_data"]["at"].asInt64() == 1'700'000'000'000);
  CHECK(payloadOn("argus.guard.v1.change")["scope"].asString() == "status");
}

TEST_CASE("two activations are two audit rows, and one camera keeps one lease")
{
  boot();
  SirenRig rig;
  static_cast<void>(rig.activate(3, 2));
  rig.clock += 4'000;
  static_cast<void>(rig.activate(3, 2));

  const auto rows = DbService::client()->execSqlSync(
      "SELECT COUNT(*) FROM change_outbox WHERE subject = "
      "'argus.guard.v1.user-action'");
  CHECK(rows[0][0].as<int64_t>() == 2);
  REQUIRE(rig.camera.calls().size() == 2);
  CHECK(rig.camera.calls()[0].commandId != rig.camera.calls()[1].commandId);
  CHECK(rig.camera.calls()[1].expiresAt > rig.camera.calls()[0].expiresAt);
}

TEST_CASE("a camera refusal is the camera's own refusal and records nothing")
{
  boot();
  SirenRig rig;
  rig.camera.rejectNext("no_driver");

  const Refusal refusal = refusalOf([&] { rig.activate(3, 2); });
  CHECK(refusal.status == 502);
  CHECK(refusal.message == "no_driver");
  CHECK(outboxPayloads().empty());
}

TEST_CASE("a camera the home does not have is a not found")
{
  boot();
  SirenRig rig;
  rig.camera.rejectNext("camera_not_found");

  CHECK(refusalOf([&] { rig.activate(99, 2); }).status == 404);
  CHECK(outboxPayloads().empty());
}

TEST_CASE("a home with no camera actions refuses before it records")
{
  boot();
  SirenRig rig;
  SirenService idle{SirenService::Dependencies{.camera = nullptr,
                                               .changes = &rig.changes,
                                               .clock = {}},
                    rig.config};

  CHECK(refusalOf([&] {
          static_cast<void>(drogon::sync_wait(idle.apply(
              {.cameraId = 3, .enabled = true, .actorUserId = 2, .actorIp = {}})));
        }).status == 503);
  CHECK(outboxPayloads().empty());
}
