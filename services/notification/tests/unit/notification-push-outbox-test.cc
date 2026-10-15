#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <test-support/app-runner.hxx>
#include <drogon/orm/DbClient.h>
#include <feature/call/repositories/call/call-repository.hxx>
#include <feature/call/services/push-delivery.hxx>
#include <shared/repositories/notification/notification-repository.hxx>
#include <shared/services/change-sink/nats-notification-change-sink.hxx>
#include <shared/services/push-delivery/push-alert-writer.hxx>
#include <shared/services/push-delivery/push-delivery-wire.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <text/base64.hxx>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at database/schema.sql"
#endif

namespace
{
using test_support::AppRunner;

constexpr const char* kOutboxDb = "notification-push-outbox-test.db";
constexpr int64_t kNow = 1800000000;
constexpr std::string_view kRecipientPublicHex =
    "5ee54ad74d51ba57c198138c2f690ea8efaa105f4f5ea7c518c9a905cbab7048";

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

std::string fromHex(std::string_view hex)
{
  std::string out;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2)
    out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(index, 2)), nullptr, 16)));
  return out;
}

std::string base64UrlOf(const std::string& bytes)
{
  std::string text = base64::encode(bytes);
  for (char& symbol : text) {
    if (symbol == '+')
      symbol = '-';
    else if (symbol == '/')
      symbol = '_';
  }
  while (!text.empty() && text.back() == '=')
    text.pop_back();
  return text;
}

class OneFcmDevice final : public PushDeviceDirectory
{
public:
  drogon::Task<std::vector<PushDevice>> devicesOf(int64_t) const override
  {
    co_return std::vector<PushDevice>{{.provider = PushProvider::Fcm,
                                       .token = "fcm-token-0001",
                                       .publicKey = base64UrlOf(fromHex(kRecipientPublicHex)),
                                       .keyId = "key-0001"}};
  }
};

int64_t scalarInt(const std::string& sql)
{
  return DbService::client()->execSqlSync(sql).front()["total"].as<int64_t>();
}

struct SharedBoot
{
  std::optional<AppRunner> runner;

  SharedBoot()
  {
    std::remove(kOutboxDb);
    std::remove((std::string(kOutboxDb) + "-wal").c_str());
    std::remove((std::string(kOutboxDb) + "-shm").c_str());
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                          .filename = kOutboxDb,
                                                          .name = "default",
                                                          .timeout = -1});
    runner.emplace();
    if (!waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
    if (!DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA))
      throw std::runtime_error("notification schema apply failed");
  }
};

SharedBoot& sharedBoot()
{
  static SharedBoot boot;
  return boot;
}

void clearTables()
{
  DbService::client()->execSqlSync("DELETE FROM change_outbox");
  DbService::client()->execSqlSync("DELETE FROM call");
  DbService::client()->execSqlSync("DELETE FROM notification_delivery");
  DbService::client()->execSqlSync("DELETE FROM notification");
}

CallCreateInput ringingRow()
{
  return {.userId = 42,
          .dedupeKey = "guard:episode:9",
          .reason = "guard",
          .urgency = "critical",
          .lang = "es",
          .title = "Puerta",
          .summary = "Hay alguien en la puerta",
          .openingLine = "Hay alguien en la puerta",
          .missedLine = "Llamada perdida",
          .data = "{}",
          .createdAt = kNow,
          .expiresAt = kNow + 120,
          .pushAfter = kNow};
}

int64_t seedRinging()
{
  return drogon::sync_wait(CallRepository().createRinging(ringingRow())).value();
}

drogon::Task<bool> commitTransaction(std::shared_ptr<drogon::orm::Transaction> transaction)
{
  co_return co_await db_transaction::Commit(std::move(transaction));
}
}

TEST_CASE("a ring written in the transaction that marks it pushed rolls back with it")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  clearTables();

  const int64_t id = seedRinging();
  const CallRepository calls;
  const CallSchema call = drogon::sync_wait(calls.findById(id)).value();
  const CallPushDelivery delivery(CallPushDeliveryDependencies{
      .devices = std::make_shared<OneFcmDevice>(),
      .outbox = std::make_shared<NatsNotificationChangeSink>(nullptr, NatsNotificationChangeSink::Config{}),
      .clock = [] { return kNow; }});

  auto transaction = drogon::sync_wait(db_transaction::begin(DbService::client()));
  CHECK(drogon::sync_wait(calls.markPushed({.id = id, .now = kNow, .client = transaction.get()})));
  CHECK(drogon::sync_wait(delivery.ring(call, transaction.get())) == 1);
  db_transaction::rollback(transaction);

  CHECK(scalarInt("SELECT COUNT(*) AS total FROM change_outbox") == 0);
  CHECK(scalarInt("SELECT COALESCE(pushed_at, 0) AS total FROM call WHERE id = " + std::to_string(id)) == 0);
}

TEST_CASE("a ring written in the transaction that marks it pushed commits with it")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  clearTables();

  const int64_t id = seedRinging();
  const CallRepository calls;
  const CallSchema call = drogon::sync_wait(calls.findById(id)).value();
  const CallPushDelivery delivery(CallPushDeliveryDependencies{
      .devices = std::make_shared<OneFcmDevice>(),
      .outbox = std::make_shared<NatsNotificationChangeSink>(nullptr, NatsNotificationChangeSink::Config{}),
      .clock = [] { return kNow; }});

  auto transaction = drogon::sync_wait(db_transaction::begin(DbService::client()));
  CHECK(drogon::sync_wait(calls.markPushed({.id = id, .now = kNow, .client = transaction.get()})));
  CHECK(drogon::sync_wait(delivery.ring(call, transaction.get())) == 1);
  CHECK(drogon::sync_wait(commitTransaction(std::move(transaction))));

  CHECK(scalarInt("SELECT COUNT(*) AS total FROM change_outbox WHERE subject = 'argus.notification.v1.push_delivery'") == 1);
  CHECK(scalarInt("SELECT COALESCE(pushed_at, 0) AS total FROM call WHERE id = " + std::to_string(id)) == kNow);
}

namespace
{
CallSchema createdRing(int64_t id)
{
  CallSchema call;
  call.id = id;
  call.userId = 42;
  call.dedupeKey = "guard:episode:9";
  call.urgency = "critical";
  call.lang = "es";
  call.title = "Puerta";
  call.createdAt = kNow;
  call.expiresAt = kNow + 120;
  return call;
}
}

TEST_CASE("a ring created and marked pushed in one transaction rolls back as one")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  clearTables();

  const CallRepository calls;
  const CallPushDelivery delivery(CallPushDeliveryDependencies{
      .devices = std::make_shared<OneFcmDevice>(),
      .outbox = std::make_shared<NatsNotificationChangeSink>(nullptr, NatsNotificationChangeSink::Config{}),
      .clock = [] { return kNow; }});

  auto transaction = drogon::sync_wait(db_transaction::begin(DbService::client()));
  CallCreateInput row = ringingRow();
  row.client = transaction.get();
  const auto id = drogon::sync_wait(calls.createRinging(row));
  REQUIRE(id.has_value());
  CHECK(drogon::sync_wait(calls.markPushed({.id = *id, .now = kNow, .client = transaction.get()})));
  CHECK(drogon::sync_wait(delivery.ring(createdRing(*id), transaction.get())) == 1);
  db_transaction::rollback(transaction);

  CHECK(scalarInt("SELECT COUNT(*) AS total FROM call") == 0);
  CHECK(scalarInt("SELECT COUNT(*) AS total FROM change_outbox") == 0);
}

TEST_CASE("a ring created and marked pushed in one transaction commits as one")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  clearTables();

  const CallRepository calls;
  const CallPushDelivery delivery(CallPushDeliveryDependencies{
      .devices = std::make_shared<OneFcmDevice>(),
      .outbox = std::make_shared<NatsNotificationChangeSink>(nullptr, NatsNotificationChangeSink::Config{}),
      .clock = [] { return kNow; }});

  auto transaction = drogon::sync_wait(db_transaction::begin(DbService::client()));
  CallCreateInput row = ringingRow();
  row.client = transaction.get();
  const auto id = drogon::sync_wait(calls.createRinging(row));
  REQUIRE(id.has_value());
  CHECK(drogon::sync_wait(calls.markPushed({.id = *id, .now = kNow, .client = transaction.get()})));
  CHECK(drogon::sync_wait(delivery.ring(createdRing(*id), transaction.get())) == 1);
  CHECK(drogon::sync_wait(commitTransaction(std::move(transaction))));

  CHECK(scalarInt("SELECT COUNT(*) AS total FROM call") == 1);
  CHECK(scalarInt("SELECT COUNT(*) AS total FROM change_outbox WHERE subject = 'argus.notification.v1.push_delivery'") == 1);
}

TEST_CASE("the same delivery id recorded twice leaves one outbox row")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  clearTables();

  const NatsNotificationChangeSink sink(nullptr, NatsNotificationChangeSink::Config{});
  const PushDeliveryRecord entry{.eventId = std::string(push_delivery_wire::kEventIdPrefix) + "0102",
                                 .payload = R"({"deliveryId":"AQI="})"};
  drogon::sync_wait(sink.record(entry));
  drogon::sync_wait(sink.record(entry));

  CHECK(scalarInt("SELECT COUNT(*) AS total FROM change_outbox WHERE event_id = 'push-delivery:0102'") == 1);
}

TEST_CASE("an alert is written with its notification, and a duplicate command writes none")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  clearTables();

  const auto alerts = std::make_shared<PushAlertWriter>(PushAlertWriterDependencies{
      .devices = std::make_shared<OneFcmDevice>(),
      .outbox = std::make_shared<NatsNotificationChangeSink>(nullptr, NatsNotificationChangeSink::Config{}),
      .clock = [] { return kNow; }});
  NotificationCreateInput row;
  row.userId = 7;
  row.type = "camera";
  row.title = "Puerta";
  row.body = "Movimiento en la puerta";
  row.data = Json::Value(Json::objectValue);
  const NotificationBatchCommitInput batch{.inputs = {row},
                                           .commandId = "cmd-alert-0001",
                                           .at = kNow,
                                           .alerts = alerts};

  const NotificationRepository notifications;
  const auto first = drogon::sync_wait(notifications.createManyWithCommand(batch));
  CHECK_FALSE(first.duplicate);
  CHECK(scalarInt("SELECT COUNT(*) AS total FROM change_outbox WHERE event_id LIKE 'push-delivery:%'") == 1);

  const auto second = drogon::sync_wait(notifications.createManyWithCommand(batch));
  CHECK(second.duplicate);
  CHECK(scalarInt("SELECT COUNT(*) AS total FROM change_outbox WHERE event_id LIKE 'push-delivery:%'") == 1);
}
