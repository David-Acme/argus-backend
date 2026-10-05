#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <nats/live-broker.hxx>
#include <nats/nats-bus.hxx>
#include <outbox/transactional-outbox.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
constexpr const char* kOutboxDb = "lib-transactional-outbox-test.db";
constexpr const char* kDefaultSubject = "argus.test.outbox.default";
constexpr const char* kRowSubject = "argus.test.outbox.row";

constexpr ErrorDefinition kRefusal{.code = ErrorCode::InternalError,
                                   .status = 500,
                                   .message = "The change could not be recorded"};

constexpr const char* kKeyedShape =
    "CREATE TABLE change_outbox ("
    "event_id TEXT NOT NULL PRIMARY KEY, "
    "fingerprint TEXT NOT NULL DEFAULT '', "
    "payload TEXT NOT NULL, "
    "status TEXT NOT NULL DEFAULT 'pending' "
    "CHECK (status IN ('pending', 'sent')), "
    "attempts INTEGER NOT NULL DEFAULT 0, "
    "created_at INTEGER NOT NULL DEFAULT 0, "
    "sent_at INTEGER NOT NULL DEFAULT 0)";

void removeDb()
{
  std::remove(kOutboxDb);
  std::remove((std::string(kOutboxDb) + "-wal").c_str());
  std::remove((std::string(kOutboxDb) + "-shm").c_str());
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
      removeDb();
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
  while (!drogon::app().isRunning() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return drogon::app().isRunning();
}

void reshape()
{
  DbService::client()->execSqlSync("DROP TABLE IF EXISTS change_outbox");
  DbService::client()->execSqlSync(kKeyedShape);
}

outbox::OutboxConfig configOf(std::string defaultSubject,
                              std::function<bool(NatsBus&)> ensureStreams)
{
  return {.label = "Test outbox",
          .client = [] { return DbService::client(); },
          .defaultSubject = std::move(defaultSubject),
          .legacyIdPrefix = "test-action:",
          .refusal = kRefusal,
          .ensureStreams = std::move(ensureStreams),
          .timing = {.retryMs = 20,
                     .maxRetryMs = 40,
                     .progressMs = 10,
                     .batch = 64},
          .retention = {.keepSentMs = 3600000,
                        .purgeEveryMs = 3600000,
                        .purgeRetryMs = 60000}};
}

std::vector<outbox::OutboxRow> pending(const outbox::TransactionalOutbox& box)
{
  return box.repository().pendingBatch(64);
}

bool waitUntil(const std::function<bool()>& done,
               std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return done();
}
}

TEST_CASE("a recorded transition lands once and a conflict is not dispatched")
{
  reshape();
  outbox::TransactionalOutbox box(nullptr, configOf(kDefaultSubject, {}));
  REQUIRE(box.migrateSchema());

  using outbox::OutboxDisposition;
  CHECK(drogon::sync_wait(box.record({.eventId = "test-change:a",
                                      .subject = kRowSubject,
                                      .payload = R"({"info":1})",
                                      .client = nullptr})) ==
        OutboxDisposition::Enqueued);
  CHECK(drogon::sync_wait(box.record({.eventId = "test-change:a",
                                      .subject = kRowSubject,
                                      .payload = R"({"info":1})",
                                      .client = nullptr})) ==
        OutboxDisposition::Replay);
  CHECK(drogon::sync_wait(box.record({.eventId = "test-change:a",
                                      .subject = kRowSubject,
                                      .payload = R"({"info":2})",
                                      .client = nullptr})) ==
        OutboxDisposition::Conflict);

  const auto rows = pending(box);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().subject == kRowSubject);
  CHECK(rows.front().payload == R"({"info":1})");

  CHECK_THROWS_AS(
      drogon::sync_wait(box.record(
          {.eventId = "test-change:big",
           .subject = kRowSubject,
           .payload = std::string(outbox::kMaxPayloadBytes + 1, 'x'),
           .client = nullptr})),
      ResponseException);
  CHECK_NOTHROW(drogon::sync_wait(box.record(
      {.eventId = "test-change:edge",
       .subject = kRowSubject,
       .payload = std::string(outbox::kMaxPayloadBytes, 'x'),
       .client = nullptr})));
  CHECK(pending(box).size() == 2);
}

TEST_CASE("an appended row mints its own msg id and refuses what the broker "
          "would refuse")
{
  reshape();
  outbox::TransactionalOutbox box(nullptr, configOf(kDefaultSubject, {}));
  REQUIRE(box.migrateSchema());

  drogon::sync_wait(box.append({.idPrefix = "test-action:",
                                .subject = kRowSubject,
                                .payload = R"({"action":"read"})",
                                .client = nullptr}));
  drogon::sync_wait(box.append({.idPrefix = "test-action:",
                                .subject = kRowSubject,
                                .payload = R"({"action":"read"})",
                                .client = nullptr}));
  const auto rows = pending(box);
  REQUIRE(rows.size() == 2);
  for (const auto& row : rows) {
    CHECK(row.eventId.rfind("test-action:", 0) == 0);
    CHECK(row.eventId.size() == std::string("test-action:").size() + 32);
  }
  CHECK(rows.front().eventId != rows.back().eventId);

  try {
    drogon::sync_wait(
        box.append({.idPrefix = "test-action:",
                    .subject = kRowSubject,
                    .payload = std::string(outbox::kMaxPayloadBytes + 1, 'x'),
                    .client = nullptr}));
    FAIL("an oversized row was accepted");
  }
  catch (const ResponseException& error) {
    CHECK(error.statusCode() == 500);
  }
  CHECK_THROWS_AS(drogon::sync_wait(box.append({.idPrefix = "test-action:",
                                                .subject = "",
                                                .payload = "{}",
                                                .client = nullptr})),
                  std::invalid_argument);
  CHECK(pending(box).size() == 2);
}

TEST_CASE("a row written inside a transaction leaves with its commit")
{
  reshape();
  outbox::TransactionalOutbox box(nullptr, configOf(kDefaultSubject, {}));
  REQUIRE(box.migrateSchema());

  drogon::sync_wait([&box]() -> drogon::Task<void> {
    const auto transaction =
        co_await db_transaction::begin(DbService::client());
    co_await box.record({.eventId = "test-change:rolled",
                         .subject = kRowSubject,
                         .payload = "{}",
                         .client = transaction.get()});
    db_transaction::rollback(transaction);
  }());
  CHECK(pending(box).empty());

  drogon::sync_wait([&box]() -> drogon::Task<void> {
    auto transaction = co_await db_transaction::begin(DbService::client());
    co_await box.record({.eventId = "test-change:kept",
                         .subject = kRowSubject,
                         .payload = "{}",
                         .client = transaction.get()});
    co_await db_transaction::Commit(std::move(transaction));
  }());
  const auto rows = pending(box);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().eventId == "test-change:kept");
}

TEST_CASE("the relay drains on request and leaves undelivered rows pending")
{
  reshape();
  outbox::TransactionalOutbox box(nullptr, configOf(kDefaultSubject, {}));
  REQUIRE(box.migrateSchema());
  CHECK(box.drained());

  box.reconcile();
  box.reconcile();
  CHECK_FALSE(box.drained());
  drogon::sync_wait(box.record({.eventId = "test-change:waiting",
                                .subject = kRowSubject,
                                .payload = "{}",
                                .client = nullptr}));
  box.requestStop();
  CHECK(waitUntil([&box] { return box.drained(); }, std::chrono::seconds(5)));
  box.requestStop();
  CHECK(box.drained());

  const auto rows = pending(box);
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().attempts == 0);
}

TEST_CASE("a live relay publishes legacy rows on the default subject with "
          "their legacy id" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  const char* url = broker.c_str();

  reshape();
  const std::string run = std::to_string(::getpid());
  const std::string stream = "argus-test-outbox-" + run;
  const std::string subject = "argus.test.outbox." + run;
  auto bus = std::make_shared<NatsBus>();
  NatsBus::Options options;
  options.url = url;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus->connect(options));

  std::mutex mutex;
  std::condition_variable arrived;
  std::vector<std::string> msgIds;
  std::atomic<bool> ensured{false};
  outbox::TransactionalOutbox box(
      bus, configOf(subject, [&ensured, &stream, &subject](NatsBus& live) {
        const bool ready = live.ensureStream({.name = stream,
                                              .subjects = {subject},
                                              .maxAgeNs = 3600000000000LL,
                                              .duplicatesNs = 120000000000LL});
        ensured.store(ready);
        return ready;
      }));
  REQUIRE(box.migrateSchema());
  DbService::client()->execSqlSync(
      "INSERT INTO change_outbox (event_id, subject, payload) "
      "VALUES ('', '', '{\"legacy\":true}')");
  const int64_t legacyRow = pending(box).front().id;

  box.reconcile();
  REQUIRE(waitUntil([&box] { return pending(box).empty(); },
                    std::chrono::seconds(10)));
  CHECK(ensured.load());

  const auto subscription = bus->subscribeDurable(
      {.stream = stream,
       .durable = "outbox-live-" + run,
       .subject = subject,
       .deliverAll = true,
       .maxDeliver = 3,
       .maxAckPending = NatsBus::kDefaultMaxAckPending,
       .handler = [&mutex, &arrived, &msgIds](
                      const NatsBus::DurableMessage& message,
                      const NatsBus::DurableSettlement& settlement) {
         {
           std::scoped_lock lock(mutex);
           msgIds.emplace_back(message.msgId);
         }
         settlement.ack();
         arrived.notify_all();
       }});
  REQUIRE(subscription.has_value());
  std::unique_lock lock(mutex);
  arrived.wait_for(lock, std::chrono::seconds(10),
                   [&msgIds] { return !msgIds.empty(); });
  REQUIRE(msgIds.size() == 1);
  CHECK(msgIds.front() == "test-action:" + std::to_string(legacyRow));
}

int main(int argc, char** argv)
{
  removeDb();
  drogon::app().setLogLevel(trantor::Logger::kFatal);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = kOutboxDb,
                                 .name = "default",
                                 .timeout = -1});
  const AppRunner runner;
  if (!waitForBoot(std::chrono::seconds(30)))
    return 1;
  doctest::Context context(argc, argv);
  return context.run();
}
