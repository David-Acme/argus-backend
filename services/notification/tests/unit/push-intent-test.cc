#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>
#include <notification/notification-delivery-sink.hxx>
#include <nats/push-intent-sink.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <shared/services/notification/push-copy.hxx>
#include <sqlite/db-service.hxx>
#include <nats/nats-push-intent-sink.hxx>
#include <nats/nats-subject.hxx>
#include <nats/nats-bus.hxx>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
int tempCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

class TempDb
{
public:
  explicit TempDb(const char* stem)
      : path_(std::string(stem) + "-" + std::to_string(::getpid()) + "-" +
              std::to_string(tempCounter()) + ".db")
  {
  }

  ~TempDb()
  {
    std::remove(path_.c_str());
    std::remove((path_ + "-wal").c_str());
    std::remove((path_ + "-shm").c_str());
  }

  const std::string& path() const { return path_; }

private:
  std::string path_;
};

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

void drain(const drogon::orm::DbClientPtr& client)
{
  auto drained = std::make_shared<std::promise<void>>();
  auto done = drained->get_future();
  client->execSqlAsync(
      "SELECT 1",
      [drained](const drogon::orm::Result&) {
        trantor::EventLoop::getEventLoopOfCurrentThread()->queueInLoop(
            [drained]() { drained->set_value(); });
      },
      [drained](const std::exception_ptr& e) {
        try {
          std::rethrow_exception(e);
        }
        catch (const std::exception& ex) {
          std::fprintf(stderr, "drain statement failed: %s\n", ex.what());
        }
        drained->set_value();
      });
  if (done.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
    throw std::runtime_error("the client's loop did not drain");
}

class ScopeDrain
{
public:
  explicit ScopeDrain(drogon::orm::DbClientPtr client)
      : client_(std::move(client))
  {
  }

  ~ScopeDrain()
  {
    try {
      drain(client_);
    }
    catch (const std::exception& ex) {
      std::fprintf(stderr, "scope drain failed: %s\n", ex.what());
    }
  }

  ScopeDrain(const ScopeDrain&) = delete;
  ScopeDrain& operator=(const ScopeDrain&) = delete;

private:
  drogon::orm::DbClientPtr client_;
};

class ArmedDeliverySink final : public NotificationDeliverySink
{
public:
  bool ensureStream() const override { return true; }

  bool publish(const NotificationDeliveryEvent&) const override
  {
    return true;
  }
};

class RecordingPushIntentSink : public push_intent::PushIntentSink
{
public:
  void publish(const PushIntent& intent) const override
  {
    if (failAfter >= 0 && static_cast<int>(recorded.size()) >= failAfter)
      throw std::runtime_error("forced push failure");
    recorded.push_back(intent);
  }

  const std::vector<PushIntent>& recordedIntents() const { return recorded; }

  mutable std::vector<PushIntent> recorded;
  mutable int failAfter{-1};
};

struct WaitForIntentsInput
{
  const RecordingPushIntentSink& sink;
  size_t expected{0};
  std::chrono::milliseconds timeout{0};
};

bool waitForIntents(const WaitForIntentsInput& input)
{
  const RecordingPushIntentSink& sink = input.sink;
  const size_t expected = input.expected;
  const std::chrono::milliseconds timeout = input.timeout;

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (sink.recordedIntents().size() >= expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return sink.recordedIntents().size() >= expected;
}
}

TEST_CASE("the push_intent gate defaults to off with no sink installed")
{
  CHECK_FALSE(push_intent::enabledFromConfig());
  CHECK(push_intent::getSink() == nullptr);
}

TEST_CASE("push_intent payloads match the subjects.md contract")
{
  const PushIntent intent{.userId = 42,
                          .notificationId = 17,
                          .type = "camera",
                          .title = "Front door",
                          .body = "Person detected",
                          .createdAtMs = 1735689600123,
                          .data = Json::Value()};
  const Json::Value json = push_intent::toJson(intent);
  CHECK_FALSE(json.isMember("data"));

  CHECK(json["userId"].as<int64_t>() == 42);
  CHECK(json["notificationId"].as<int64_t>() == 17);
  CHECK(json["type"].asString() == "camera");
  CHECK(json["title"].asString() == "Front door");
  CHECK(json["body"].asString() == "Person detected");
  CHECK(json["createdAt"].as<int64_t>() == 1735689600123);

  Json::Value callData(Json::objectValue);
  callData["kind"] = "call";
  callData["deepLink"] = "argus://call?callId=call-9";
  PushIntent ringing = intent;
  ringing.data = callData;
  const Json::Value withData = push_intent::toJson(ringing);
  CHECK(withData["data"]["deepLink"].asString() ==
        "argus://call?callId=call-9");

  CHECK(nats_subject::isValidSubject(nats_subject::kNotificationPushIntent,
                                     nats_subject::SubjectKind::Publish));
  CHECK(nats_subject::kNotificationPushIntent ==
        std::string("argus.notification.v1.push_intent"));
}

TEST_CASE("push copy carries no household detail, only a generic line")
{
  Json::Value data(Json::objectValue);
  data["kind"] = "guard_episode";
  data["urgency"] = "critical";
  data["lang"] = "en";
  data["cameraName"] = "Patio";
  data["personName"] = "Marta";
  const Json::Value minimal =
      push_copy::minimalData({.notificationId = 9, .data = data});
  CHECK(minimal.size() == 3);
  CHECK(minimal["notificationId"].asInt64() == 9);
  CHECK(minimal["kind"].asString() == "guard_episode");
  CHECK(minimal["urgency"].asString() == "critical");
  CHECK(push_copy::langOf(data) == "en");
  const PushCopy urgent = push_copy::render(
      {.lang = "en", .urgency = "critical", .call = false});
  CHECK(urgent.title == "Argus");
  CHECK(urgent.body == "There is an important alert. Open Argus to see it.");
  const PushCopy call =
      push_copy::render({.lang = "es", .urgency = "critical", .call = true});
  CHECK(call.title == "Argus te está llamando");
  CHECK(call.body.find("Patio") == std::string::npos);
  CHECK(push_copy::langOf(Json::Value(Json::objectValue)) == "es");
  CHECK(push_copy::urgencyOf(Json::Value(Json::objectValue)) == "active");
}

TEST_CASE("a publish on an unconnected bus is a warn, not a crash")
{
  const std::shared_ptr<NatsBus> bus = std::make_shared<NatsBus>();
  const NatsPushIntentSink sink(bus);
  REQUIRE_FALSE(bus->isConnected());
  CHECK_FALSE(bus->publish(nats_subject::kNotificationPushIntent, "{}"));
  CHECK_NOTHROW(sink.publish(PushIntent{.userId = 1,
                                        .notificationId = 2,
                                        .type = "system",
                                        .title = "",
                                        .body = "",
                                        .createdAtMs = 0,
                                        .data = Json::Value(Json::objectValue)}));
  CHECK_FALSE(bus->isConnected());
}

TEST_CASE("the create path publishes one intent per row")
{
  const TempDb db("push-intent-test");
  auto client =
      drogon::orm::DbClient::newSqlite3Client(
          std::string("filename=") + db.path(), 1);
  const ScopeDrain guard(client);
  DbService::applyPragmas(client);
  client->execSqlSync(
      "CREATE TABLE notification ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "user_id INTEGER NOT NULL, "
      "type TEXT NOT NULL DEFAULT 'system', "
      "title TEXT NOT NULL DEFAULT '', body TEXT NOT NULL DEFAULT '', "
      "data TEXT NOT NULL DEFAULT '{}', "
      "is_read INTEGER NOT NULL DEFAULT 0, "
      "read_at INTEGER, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  client->execSqlSync(
      "CREATE TABLE notification_command ("
      "command_id TEXT NOT NULL PRIMARY KEY, "
      "expected_count INTEGER NOT NULL DEFAULT 0, "
      "fingerprint TEXT NOT NULL DEFAULT '', "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  client->execSqlSync(
      "CREATE TABLE notification_delivery ("
      "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
      "notification_id INTEGER NOT NULL, "
      "user_id INTEGER NOT NULL DEFAULT 0, "
      "status TEXT NOT NULL DEFAULT 'pending', "
      "attempts INTEGER NOT NULL DEFAULT 0, "
      "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')), "
      "sent_at INTEGER NOT NULL DEFAULT 0, "
      "acked_at INTEGER NOT NULL DEFAULT 0, "
      "created_ms INTEGER NOT NULL DEFAULT 0, "
      "sent_ms INTEGER NOT NULL DEFAULT 0, "
      "acked_ms INTEGER NOT NULL DEFAULT 0, "
      "claimed_at INTEGER NOT NULL DEFAULT 0, "
      "UNIQUE (notification_id))");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  drogon::app().registerBeginningAdvice([] { DbService::applyPragmas(); });

  const AppRunner app;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  CHECK(client->execSqlSync("PRAGMA busy_timeout").front()["timeout"]
            .as<int64_t>() == 5000);

  const auto deliverySink = std::make_shared<ArmedDeliverySink>();
  const auto sink = std::make_shared<RecordingPushIntentSink>();

  NotificationBatchInput batch;
  batch.userIds = {1, 2};
  batch.notification.type = "camera";
  batch.notification.title = "Front door";
  batch.notification.body = "Person detected";
  batch.commandId = "push-intent-test";
  const NotificationService service({.deliverySink = deliverySink,
                                     .pushSink = sink,
                                     .pushRequired = false});
  drogon::sync_wait(service.createManyAndEmit(batch));

  REQUIRE(waitForIntents({.sink = *sink,
                          .expected = 2,
                          .timeout = std::chrono::seconds(10)}));
  REQUIRE(sink->recordedIntents().size() == 2);
  CHECK(sink->recordedIntents()[0].userId == 1);
  CHECK(sink->recordedIntents()[0].type == "camera");
  CHECK(sink->recordedIntents()[0].title == "Argus");
  CHECK(sink->recordedIntents()[0].body ==
        "Tienes un aviso nuevo. Abre Argus para verlo.");
  CHECK(sink->recordedIntents()[0].data["notificationId"].asInt64() ==
        sink->recordedIntents()[0].notificationId);
  CHECK(sink->recordedIntents()[0].data["urgency"].asString() == "active");
  CHECK(sink->recordedIntents()[0].data.size() == 3);
  CHECK(sink->recordedIntents()[0].createdAtMs > 0);
  CHECK(sink->recordedIntents()[1].userId == 2);
  CHECK(sink->recordedIntents()[0].notificationId !=
        sink->recordedIntents()[1].notificationId);

  const NotificationCreateOutcome duplicate =
      drogon::sync_wait(service.createManyAndEmit(batch));
  CHECK(duplicate.duplicate);
  CHECK(duplicate.createdCount == 2);
  CHECK(sink->recordedIntents().size() == 2);
  CHECK(client->execSqlSync("SELECT COUNT(*) AS total FROM notification")
            .front()["total"]
            .as<int64_t>() == 2);
  CHECK(client->execSqlSync(
            "SELECT COUNT(*) AS total FROM notification_delivery "
            "WHERE status = 'sent'")
            .front()["total"]
            .as<int64_t>() == 2);

  client->execSqlSync(
      "ALTER TABLE notification_command RENAME TO notification_command_backup");
  NotificationBatchInput commandFailure = batch;
  commandFailure.commandId = "push-intent-command-fault";
  bool commandThrew = false;
  try {
    drogon::sync_wait(service.createManyAndEmit(commandFailure));
  }
  catch (const std::exception&) {
    commandThrew = true;
  }
  CHECK(commandThrew);
  client->execSqlSync(
      "ALTER TABLE notification_command_backup RENAME TO notification_command");

  client->execSqlSync(
      "ALTER TABLE notification RENAME TO notification_backup");
  NotificationBatchInput faultBatch = batch;
  faultBatch.commandId = "push-intent-fault";
  bool threw = false;
  try {
    drogon::sync_wait(service.createManyAndEmit(faultBatch));
  }
  catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(client->execSqlSync("SELECT COUNT(*) AS total FROM notification_command "
                            "WHERE command_id = 'push-intent-fault'")
            .front()["total"]
            .as<int64_t>() == 0);
  client->execSqlSync(
      "ALTER TABLE notification_backup RENAME TO notification");

  const NotificationCreateOutcome recovered =
      drogon::sync_wait(service.createManyAndEmit(faultBatch));
  CHECK_FALSE(recovered.duplicate);
  CHECK(recovered.createdCount == 2);
  CHECK(client->execSqlSync("SELECT COUNT(*) AS total FROM notification")
            .front()["total"]
            .as<int64_t>() == 4);

  NotificationBatchInput crashBatch = batch;
  crashBatch.commandId = "push-intent-crash";
  sink->failAfter = static_cast<int>(sink->recordedIntents().size());
  bool crashThrew = false;
  try {
    drogon::sync_wait(service.createManyAndEmit(crashBatch));
  }
  catch (const std::exception&) {
    crashThrew = true;
  }
  CHECK(crashThrew);
  CHECK(client->execSqlSync("SELECT COUNT(*) AS total FROM notification")
            .front()["total"]
            .as<int64_t>() == 6);
  CHECK(client->execSqlSync("SELECT COUNT(*) AS total FROM notification_delivery "
                            "WHERE status = 'sent'")
            .front()["total"]
            .as<int64_t>() == 4);
  CHECK(client->execSqlSync("SELECT COUNT(*) AS total FROM notification_delivery "
                            "WHERE status = 'pending'")
            .front()["total"]
            .as<int64_t>() == 2);

  sink->failAfter = -1;
  const size_t beforeRecovery = sink->recordedIntents().size();
  drogon::sync_wait(service.deliverPending());
  CHECK(sink->recordedIntents().size() == beforeRecovery + 2);
  CHECK(client->execSqlSync("SELECT COUNT(*) AS total FROM notification_delivery "
                            "WHERE status = 'pending'")
            .front()["total"]
            .as<int64_t>() == 0);
  CHECK(client->execSqlSync("SELECT COUNT(*) AS total FROM notification")
            .front()["total"]
            .as<int64_t>() == 6);

  const auto commands = [&client] {
    return client->execSqlSync("SELECT COUNT(*) AS total FROM notification_command")
        .front()["total"]
        .as<int64_t>();
  };
  const int64_t kept = commands();
  REQUIRE(kept >= 2);
  client->execSqlSync("UPDATE notification_command SET created_at = 100 "
                      "WHERE command_id = 'push-intent-fault'");
  const NotificationRepository repository;
  CHECK(drogon::sync_wait(repository.purgeCommands(1000)) == 1);
  CHECK(commands() == kept - 1);

  drain(client);
}
