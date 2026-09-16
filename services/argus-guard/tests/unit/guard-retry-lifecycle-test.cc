#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <atomic>
#include <camera/camera-action-client.hxx>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <guard-repository.hxx>
#include <guard-schema.hxx>
#include <guard-service.hxx>
#include <identity/identity-client.hxx>
#include <json/value.h>
#include <notification/notification-client.hxx>
#include <optional>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_GUARD_SCHEMA_PATH
#error "ARGUS_GUARD_SCHEMA_PATH must point at database/schema.sql"
#endif

#include "temp-db.hxx"
#include "wait-for-boot.hxx"

using guard_test::TempDb;
using guard_test::waitForBoot;

namespace
{
std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

struct WaitForScalarInput
{
  std::string sql;
  std::string expected;
  std::chrono::milliseconds timeout{};
};

bool waitForScalar(const WaitForScalarInput& input)
{
  const auto deadline = std::chrono::steady_clock::now() + input.timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (scalar(input.sql) == input.expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return scalar(input.sql) == input.expected;
}

Json::Value retryObservation(const std::string& eventId, int64_t cameraId = 1)
{
  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 2;
  event["eventId"] = eventId;
  event["cameraId"] = Json::Int64(cameraId);
  event["rule"] = "person_day";
  event["severity"] = "info";
  event["trackId"] = Json::Int64(1);
  Json::Value object(Json::objectValue);
  object["class"] = "person";
  object["identity"] = "unknown";
  object["trackId"] = Json::Int64(1);
  Json::Value bbox(Json::objectValue);
  bbox["w"] = 32.0;
  bbox["h"] = 32.0;
  object["bbox"] = bbox;
  Json::Value objects(Json::arrayValue);
  objects.append(object);
  event["objects"] = objects;
  return event;
}

class FakeIdentity final : public IdentityClient
{
public:
  FakeIdentity() : IdentityClient("127.0.0.1:1", "fleet") {}

  std::optional<std::vector<int64_t>> listNotifiableUsers() const override
  {
    return std::vector<int64_t>{7};
  }
};

class CountingNotifications final : public NotificationClient
{
public:
  CountingNotifications()
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::sdk::CallerIdentity&) const override
  {
    calls.fetch_add(1);
    NotificationCreateResult result;
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  mutable std::atomic<int> calls{0};
};

class FlakyNotifications final : public NotificationClient
{
public:
  FlakyNotifications()
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::sdk::CallerIdentity&) const override
  {
    const int call = calls.fetch_add(1);
    NotificationCreateResult result;
    if (call == 0) {
      result.outcome = NotificationRpcOutcome::Unavailable;
      return result;
    }
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  mutable std::atomic<int> calls{0};
};

class FakeCameraActions final : public CameraActionClient
{
public:
  FakeCameraActions()
      : CameraActionClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  CameraCommandResult announce(const CameraAnnounceInput&) const override
  {
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::SUCCEEDED;
    return result;
  }

  CameraCommandResult listen(const CameraListenInput&) const override
  {
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::INDETERMINATE;
    return result;
  }

  CameraCommandResult alarm(const CameraAlarmInput&) const override
  {
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::SUCCEEDED;
    return result;
  }

  CameraCommandResult setSiren(const CameraSirenInput&) const override
  {
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::REJECTED;
    return result;
  }

  std::optional<CameraCrop> personCrop(
      const CameraPersonCropInput&) const override
  {
    return std::nullopt;
  }
};

class SlowNotifications final : public NotificationClient
{
public:
  SlowNotifications()
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::sdk::CallerIdentity&) const override
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    calls.fetch_add(1);
    NotificationCreateResult result;
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  mutable std::atomic<int> calls{0};
};

GuardService::Config baseConfig()
{
  GuardService::Config config;
  config.enabled = true;
  config.stagingEnabled = false;
  config.notifyLevel = 1;
  config.retryLeaseMs = 300;
  return config;
}
} // namespace

TEST_CASE("durable retries survive destroy, races and teardown")
{
  const TempDb db("guard-retry-lifecycle-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH));

  const GuardRepository repository;
  FakeIdentity identity;
  FakeCameraActions actions;
  CountingNotifications notifications;

  std::atomic<bool> failIncident{true};
  auto failPoint = [&failIncident](const std::string& name) {
    return failIncident.load() && name == "after_incident";
  };

  {
    GuardService::Config config = baseConfig();
    config.failPoint = failPoint;
    GuardService crashing({.bus = nullptr,
                           .identity = &identity,
                           .notifications = &notifications,
                           .actions = &actions,
                           .assessment = nullptr},
                          config);
    crashing.start();
    const Json::Value event = retryObservation("retry-crash:1");
    REQUIRE(drogon::sync_wait(repository.claimObservation(
                {.eventId = "retry-crash:1",
                 .cameraId = 1,
                 .observationId = "retry-crash:1",
                 .receivedAt = 100,
                 .payload = json_util::toString(event),
                 .delivered = 1}))
                .kind == ObservationClaimKind::New);
    REQUIRE(drogon::sync_wait(repository.setInboxRetry(
        {.eventId = "retry-crash:1",
         .payload = json_util::toString(event),
         .retryAt = 101,
         .at = 100})));
    REQUIRE(waitForScalar({.sql = "SELECT COUNT(*) FROM guard_incident WHERE "
                                  "event_id = 'retry-crash:1'",
                           .expected = "1",
                           .timeout = std::chrono::seconds(10)}));
  }

  {
    GuardService::Config config = baseConfig();
    GuardService recovered({.bus = nullptr,
                            .identity = &identity,
                            .notifications = &notifications,
                            .actions = &actions,
                            .assessment = nullptr},
                           config);
    recovered.start();
    REQUIRE(waitForScalar({.sql = "SELECT status FROM guard_observation_inbox "
                                  "WHERE event_id = 'retry-crash:1'",
                           .expected = "completed",
                           .timeout = std::chrono::seconds(15)}));
    CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE event_id = "
                 "'retry-crash:1'") == "1");
    CHECK(notifications.calls.load() == 1);
    CHECK(scalar("SELECT local_retries FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-crash:1'") == "1");
    CHECK(scalar("SELECT attempts FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-crash:1'") == "1");
  }

  std::atomic<bool> failSchedule{true};
  auto schedulePoint = [&failSchedule](const std::string& name) {
    return failSchedule.load() && name == "after_schedule";
  };

  {
    FlakyNotifications flaky;
    GuardService::Config config = baseConfig();
    config.failPoint = schedulePoint;
    GuardService service({.bus = nullptr,
                          .identity = &identity,
                          .notifications = &flaky,
                          .actions = &actions,
                          .assessment = nullptr},
                         config);
    service.start();
    bool threw = false;
    try {
      drogon::sync_wait(service.handle(retryObservation("retry-ack:1", 2), 1));
    }
    catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
    CHECK(scalar("SELECT retry_at > 0 FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-ack:1'") == "1");
    CHECK(scalar("SELECT local_retries FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-ack:1'") == "1");
    CHECK(drogon::sync_wait(service.handle(retryObservation("retry-ack:1", 2),
                                            2)));
    CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE event_id = "
                 "'retry-ack:1'") == "1");
    CHECK(scalar("SELECT attempts FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-ack:1'") == "2");
    CHECK(scalar("SELECT local_retries FROM guard_observation_inbox WHERE "
                 "event_id = 'retry-ack:1'") == "1");
    failSchedule.store(false);
    REQUIRE(waitForScalar({.sql = "SELECT status FROM guard_observation_inbox "
                                  "WHERE event_id = 'retry-ack:1'",
                           .expected = "completed",
                           .timeout = std::chrono::seconds(15)}));
    CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE event_id = "
                 "'retry-ack:1'") == "1");
  }

  {
    GuardService::Config config = baseConfig();
    GuardService service({.bus = nullptr,
                          .identity = &identity,
                          .notifications = &notifications,
                          .actions = &actions,
                          .assessment = nullptr},
                         config);
    const int callsBefore = notifications.calls.load();
    const Json::Value first = retryObservation("retry-race:1", 3);
    const Json::Value second = retryObservation("retry-race:1", 3);
    std::atomic<bool> firstOk{false};
    std::atomic<bool> secondOk{false};
    std::thread left([&] {
      try {
        firstOk.store(drogon::sync_wait(service.handle(first, 1)));
      }
      catch (const std::exception&) {
      }
    });
    std::thread right([&] {
      try {
        secondOk.store(drogon::sync_wait(service.handle(second, 2)));
      }
      catch (const std::exception&) {
      }
    });
    left.join();
    right.join();
    CHECK(firstOk.load());
    CHECK(secondOk.load());
    CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE event_id = "
                 "'retry-race:1'") == "1");
    CHECK(notifications.calls.load() == callsBefore + 1);
  }

  {
    SlowNotifications slow;
    const int callsBefore = notifications.calls.load();
    std::thread worker;
    {
      GuardService::Config config = baseConfig();
      GuardService service({.bus = nullptr,
                            .identity = &identity,
                            .notifications = &slow,
                            .actions = &actions,
                            .assessment = nullptr},
                           config);
      worker = std::thread([&service] {
        try {
          drogon::sync_wait(
              service.handle(retryObservation("retry-teardown:1", 4), 1));
        }
        catch (const std::exception&) {
        }
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    worker.join();
    CHECK(slow.calls.load() == 1);
    CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE event_id "
                 "= 'retry-teardown:1'") == "completed");

    GuardService::Config config = baseConfig();
    GuardService rebuilt({.bus = nullptr,
                          .identity = &identity,
                          .notifications = &notifications,
                          .actions = &actions,
                          .assessment = nullptr},
                         config);
    CHECK(drogon::sync_wait(
        rebuilt.handle(retryObservation("retry-teardown:1", 4), 2)));
    CHECK(notifications.calls.load() == callsBefore);
  }

  {
    NatsBus bus;
    NatsBus::Options options;
    options.url = "nats://127.0.0.1:59999";
    options.reconnectWaitMs = 50;
    options.maxReconnects = 1;
    bus.connect(options);
    {
      GuardService::Config config = baseConfig();
      GuardService service({.bus = &bus,
                            .identity = &identity,
                            .notifications = &notifications,
                            .actions = &actions,
                            .assessment = nullptr},
                           config);
      service.start();
      std::this_thread::sleep_for(std::chrono::seconds(6));
    }
    std::this_thread::sleep_for(std::chrono::seconds(6));
    GuardService::Config config = baseConfig();
    GuardService service({.bus = nullptr,
                          .identity = &identity,
                          .notifications = &notifications,
                          .actions = &actions,
                          .assessment = nullptr},
                         config);
    CHECK(drogon::sync_wait(
        service.handle(retryObservation("retry-post-teardown:1", 5), 1)));
    bus.drain();
  }

  drogon::app().quit();
  runner.join();
}
