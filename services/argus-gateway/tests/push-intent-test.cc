#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/contracts/push-intent-sink.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/nats/nats-push-intent-sink.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>

#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kPushDb = "push-intent-test.db";

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

class RecordingPushIntentSink : public push_intent::PushIntentSink
{
public:
  void publish(const PushIntent& intent) const override
  {
    recorded.push_back(intent);
  }

  const std::vector<PushIntent>& recordedIntents() const { return recorded; }

private:
  mutable std::vector<PushIntent> recorded;
};

bool waitForIntents(const RecordingPushIntentSink& sink, size_t expected,
                    std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (sink.recordedIntents().size() >= expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return sink.recordedIntents().size() >= expected;
}
} // namespace

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
                          .createdAtMs = 1735689600123};
  const Json::Value json = push_intent::toJson(intent);

  CHECK(json["userId"].as<int64_t>() == 42);
  CHECK(json["notificationId"].as<int64_t>() == 17);
  CHECK(json["type"].asString() == "camera");
  CHECK(json["title"].asString() == "Front door");
  CHECK(json["body"].asString() == "Person detected");
  CHECK(json["createdAt"].as<int64_t>() == 1735689600123);

  CHECK(nats_subject::isValidSubject(nats_subject::kNotificationPushIntent,
                                     nats_subject::SubjectKind::Publish));
  // The gateway's sync wildcard does not match the intent subject: it is
  // never re-emitted to /sync.
  CHECK(nats_subject::kNotificationPushIntent ==
        std::string("argus.notification.v1.push_intent"));
}

TEST_CASE("a publish on an unconnected bus is a warn, not a crash")
{
  const std::shared_ptr<NatsBus> bus = std::make_shared<NatsBus>();
  const NatsPushIntentSink sink(bus);
  sink.publish(PushIntent{.userId = 1,
                          .notificationId = 2,
                          .type = "system",
                          .title = "",
                          .body = "",
                          .createdAtMs = 0});
}

TEST_CASE("the createAndEmitMany hook publishes one intent per row")
{
  std::remove(kPushDb);
  auto client =
      drogon::orm::DbClient::newSqlite3Client(
          std::string("filename=") + kPushDb, 1);
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
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kPushDb, "default", -1});

  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  const RecordingPushIntentSink sink;
  push_intent::setSink(&sink);

  NotificationCreateInput input;
  input.type = "camera";
  input.title = "Front door";
  input.body = "Person detected";
  const NotificationService service;
  drogon::sync_wait(service.createAndEmitMany({1, 2}, input));

  REQUIRE(waitForIntents(sink, 2, std::chrono::seconds(10)));
  REQUIRE(sink.recordedIntents().size() == 2);
  CHECK(sink.recordedIntents()[0].userId == 1);
  CHECK(sink.recordedIntents()[0].type == "camera");
  CHECK(sink.recordedIntents()[0].title == "Front door");
  CHECK(sink.recordedIntents()[0].body == "Person detected");
  CHECK(sink.recordedIntents()[0].createdAtMs > 0);
  CHECK(sink.recordedIntents()[1].userId == 2);
  // Distinct rows: the intents mirror the persisted ids.
  CHECK(sink.recordedIntents()[0].notificationId !=
        sink.recordedIntents()[1].notificationId);

  push_intent::setSink(nullptr);
  drogon::app().quit();
  runner.join();
  std::remove(kPushDb);
}
