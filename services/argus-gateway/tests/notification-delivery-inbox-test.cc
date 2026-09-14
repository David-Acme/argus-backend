#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <sync/notification-delivery-consumer.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA_PATH
#error "ARGUS_IDENTITY_SCHEMA_PATH must point at the identity schema.sql"
#endif

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

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

NotificationDeliveryEvent deliveryEvent(int64_t deliveryId)
{
  return {.deliveryId = deliveryId,
          .notificationId = 1000 + deliveryId,
          .userId = 7,
          .type = "camera",
          .title = "Front door",
          .body = "Person detected",
          .data = Json::Value(Json::objectValue),
          .createdAt = 1700000000};
}

NotificationDeliveryConsumer::Config consumerConfig()
{
  return {.stream = "test-stream",
          .durable = "test-durable",
          .subject = "test-subject",
          .maxDeliver = 10,
          .poisonMaxAttempts = 3};
}
} // namespace

TEST_CASE("the gateway delivery inbox is durable, exact and fail-closed")
{
  CHECK(nats_subject::isValidSubject(nats_subject::kNotificationDelivery,
                                     nats_subject::SubjectKind::Publish));
  CHECK(nats_subject::isValidSubject(nats_subject::kNotificationDelivery,
                                     nats_subject::SubjectKind::Subscribe));
  CHECK(nats_subject::kNotificationDelivery ==
        std::string("argus.notification.v1.delivery"));
  CHECK(notificationDeliveryReceiptToString(
            NotificationDeliveryReceipt::Received) == "received");
  CHECK(notificationDeliveryReceiptToString(
            NotificationDeliveryReceipt::Dispatched) == "dispatched");
  CHECK(notificationDeliveryReceiptToString(
            NotificationDeliveryReceipt::Conflict) == "conflict");
  CHECK(notificationDeliveryReceiptToString(
            NotificationDeliveryReceipt::DeadLettered) == "dead_lettered");
  CHECK(notificationDeliveryReceiptFromString("dispatched") ==
        NotificationDeliveryReceipt::Dispatched);
  CHECK_FALSE(
      notificationDeliveryReceiptFromString("bogus").has_value());

  const TempDb db("notification-delivery-inbox-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA_PATH));
  DbService::client()->execSqlSync(
      "INSERT INTO user (name, last_name, role, lang, is_active, created_at) "
      "VALUES ('Ada', 'Lovelace', 'owner', 'es', 1, 1700000000)");
  DbService::client()->execSqlSync("DROP TABLE notification_delivery_inbox");
  CHECK(scalar("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND "
               "name = 'notification_delivery_inbox'") == "0");
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA_PATH));
  CHECK(scalar("SELECT COUNT(*) FROM user") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND "
               "name = 'notification_delivery_inbox'") == "1");
  CHECK(scalar("SELECT sql FROM sqlite_master WHERE type = 'table' AND name "
               "= 'notification_delivery_inbox'")
            .find("'dead_lettered'") != std::string::npos);

  const DeliveryInboxRepository repository;
  std::vector<int64_t> dispatched;
  NotificationDeliveryConsumer consumer(
      {.bus = nullptr,
       .dispatch =
           [&dispatched](const NotificationDeliveryEvent& event) {
             dispatched.push_back(event.deliveryId);
           }},
      consumerConfig());

  CHECK(drogon::sync_wait(consumer.handle(deliveryEvent(1))) ==
        DeliveryDisposition::Ack);
  CHECK(drogon::sync_wait(consumer.handle(deliveryEvent(1))) ==
        DeliveryDisposition::Ack);
  CHECK(dispatched.size() == 1);
  CHECK(dispatched[0] == 1);
  CHECK(scalar("SELECT status FROM notification_delivery_inbox "
               "WHERE delivery_id = 1") == "dispatched");

  const auto receipt =
      drogon::sync_wait(repository.receive({.event = deliveryEvent(2),
                                            .at = 1700000001}));
  CHECK_FALSE(receipt.duplicate);
  CHECK(drogon::sync_wait(consumer.handle(deliveryEvent(2))) ==
        DeliveryDisposition::Ack);
  CHECK(dispatched.size() == 2);
  CHECK(scalar("SELECT COUNT(*) FROM notification_delivery_inbox "
               "WHERE delivery_id = 2") == "1");
  CHECK(scalar("SELECT status FROM notification_delivery_inbox "
               "WHERE delivery_id = 2") == "dispatched");

  const auto preCrash = deliveryEvent(3);
  CHECK(drogon::sync_wait(repository.receive({.event = preCrash,
                                              .at = 1700000003}))
            .duplicate == false);
  dispatched.push_back(preCrash.deliveryId);
  CHECK(drogon::sync_wait(consumer.handle(preCrash)) ==
        DeliveryDisposition::Ack);
  CHECK(dispatched.size() == 4);
  CHECK(scalar("SELECT COUNT(*) FROM notification_delivery_inbox "
               "WHERE delivery_id = 3") == "1");
  CHECK(scalar("SELECT status FROM notification_delivery_inbox "
               "WHERE delivery_id = 3") == "dispatched");

  auto tampered = deliveryEvent(11);
  tampered.title = "Tampered title";
  CHECK(drogon::sync_wait(consumer.handle(deliveryEvent(11))) ==
        DeliveryDisposition::Ack);
  CHECK(drogon::sync_wait(consumer.handle(tampered)) ==
        DeliveryDisposition::Ack);
  CHECK(dispatched.size() == 5);
  CHECK(scalar("SELECT status FROM notification_delivery_inbox "
               "WHERE delivery_id = 11") == "conflict");

  DbService::client()->execSqlSync(
      "DROP TABLE notification_delivery_inbox");
  DbService::client()->execSqlSync(
      "CREATE TABLE notification_delivery_inbox (delivery_id INTEGER NOT "
      "NULL PRIMARY KEY, notification_id INTEGER NOT NULL DEFAULT 0, user_id "
      "INTEGER NOT NULL DEFAULT 0, fingerprint TEXT NOT NULL DEFAULT '', "
      "attempts INTEGER NOT NULL DEFAULT 0, status TEXT NOT NULL DEFAULT "
      "'received', created_at INTEGER NOT NULL DEFAULT "
      "(strftime('%s', 'now')), updated_at INTEGER NOT NULL DEFAULT "
      "(strftime('%s', 'now')))");
  DbService::client()->execSqlSync(
      "INSERT INTO notification_delivery_inbox (delivery_id, "
      "notification_id, user_id, status, created_at, updated_at) VALUES (12, "
      "1012, 7, 'bogus', 1700000002, 1700000002)");
  CHECK(drogon::sync_wait(consumer.handle(deliveryEvent(12))) ==
        DeliveryDisposition::Ack);
  CHECK(dispatched.size() == 5);
  CHECK(scalar("SELECT status FROM notification_delivery_inbox "
               "WHERE delivery_id = 12") == "dead_lettered");

  NotificationDeliveryConsumer poison(
      {.bus = nullptr,
       .dispatch =
           [](const NotificationDeliveryEvent&) {
             throw std::runtime_error("poison dispatch");
           }},
      {.stream = "test-stream",
       .durable = "test-durable",
       .subject = "test-subject",
       .maxDeliver = 10,
       .poisonMaxAttempts = 2});
  CHECK(drogon::sync_wait(poison.handle(deliveryEvent(21))) ==
        DeliveryDisposition::Nak);
  CHECK(drogon::sync_wait(poison.handle(deliveryEvent(21))) ==
        DeliveryDisposition::Term);
  CHECK(scalar("SELECT status FROM notification_delivery_inbox "
               "WHERE delivery_id = 21") == "dead_lettered");
  CHECK(scalar("SELECT attempts FROM notification_delivery_inbox "
               "WHERE delivery_id = 21") == "2");

  CHECK(drogon::sync_wait(consumer.handlePayload("not-json")) ==
        DeliveryDisposition::Term);
  CHECK(drogon::sync_wait(consumer.handlePayload("{\"deliveryId\":0}")) ==
        DeliveryDisposition::Term);

  drogon::app().quit();
  runner.join();
}
