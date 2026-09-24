#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <camera/nats-camera-change-sink.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <shared/repositories/change-outbox/change-outbox-status.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>
#include <nats/nats-bus.hxx>

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
constexpr const char* kSinkDb = "camera-change-sink-test.db";

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

ChangeOutboxRow pendingRow(const std::vector<ChangeOutboxRow>& rows)
{
  REQUIRE(!rows.empty());
  return rows.empty() ? ChangeOutboxRow{} : rows.front();
}

bool hasPending(const ChangeOutboxRepository& outbox)
{
  return !outbox.pendingBatch(1).empty();
}

int64_t firstInteger(const drogon::orm::Result& rows)
{
  return rows.empty() ? 0 : rows.front()[0].as<int64_t>();
}

int64_t outboxWatermark()
{
  return firstInteger(DbService::client()->execSqlSync(
      "SELECT COALESCE(MAX(rowid), 0) FROM change_outbox"));
}

int64_t rowsAfter(int64_t watermark)
{
  return firstInteger(DbService::client()->execSqlSync(
      "SELECT COUNT(*) FROM change_outbox WHERE rowid > ?", watermark));
}

int64_t sentRowsAfter(int64_t watermark)
{
  return firstInteger(DbService::client()->execSqlSync(
      "SELECT COUNT(*) FROM change_outbox WHERE rowid > ? AND status = ?",
      watermark, changeOutboxStatusToString(ChangeOutboxStatus::Sent)));
}

bool waitForDrain(const NatsCameraChangeSink& sink,
                  std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (sink.drained())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return sink.drained();
}

SocketEmitDto addCamera(int64_t id, const std::string& name)
{
  SocketEmitDto body;
  body.operation = SyncOperation::Add;
  body.option = TableName::Camera;
  body.obj["id"] = static_cast<Json::Int64>(id);
  body.obj["name"] = name;
  return body;
}
}

TEST_CASE("the change sink lands every transition in the durable outbox")
{
  std::remove(kSinkDb);
  std::remove((std::string(kSinkDb) + "-wal").c_str());
  std::remove((std::string(kSinkDb) + "-shm").c_str());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = kSinkDb,
                                 .name = "default",
                                 .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_CAMERA_SCHEMA_PATH));

  ChangeOutboxRepository outbox;
  const SocketEmitDto add = addCamera(7, "patio");

  {
    NatsCameraChangeSink sink(
        nullptr, NatsCameraChangeSink::Config{.retryMs = 20,
                                              .publishSubject = {},
                                              .streamName = {}});
    CHECK(sink.drained());
    drogon::sync_wait(sink.emitModule(
        {.table = TableName::Camera, .body = add, .client = nullptr}));

    const ChangeOutboxRow created = pendingRow(outbox.pendingBatch(1));
    CHECK(created.eventId ==
          change_outbox_key::eventId(
              {.table = "camera",
               .recordId = 7,
               .discriminator = json_util::toString(add.toJson())}));
    CHECK(created.payload == json_util::toString(add.toJson()));
    CHECK(created.eventId.size() == 46);

    CHECK(outbox.markSent(created.eventId, 1000));
    drogon::sync_wait(sink.emitModule(
        {.table = TableName::Camera, .body = add, .client = nullptr}));
    CHECK_FALSE(hasPending(outbox));

    ModuleAuditInput audit;
    audit.recordId = 7;
    audit.tableName = TableName::Camera;
    audit.before["id"] = 7;
    audit.before["name"] = "patio";
    audit.after["id"] = 7;
    audit.after["name"] = "porch";
    drogon::sync_wait(sink.publishAudit(audit));

    const ChangeOutboxRow audited = pendingRow(outbox.pendingBatch(1));
    CHECK(audited.eventId.rfind("camera-change:", 0) == 0);
    CHECK(audited.eventId.size() == 46);
    CHECK(audited.payload.find("\"kind\":\"audit\"") != std::string::npos);
    CHECK(audited.payload.find("porch") != std::string::npos);
    CHECK(outbox.markSent(audited.eventId, 2000));

    bool repeated = false;
    for (int attempt = 0; attempt < 50 && !repeated; ++attempt) {
      drogon::sync_wait(sink.publishAudit(audit));
      repeated = hasPending(outbox);
      if (!repeated)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(repeated);
    const ChangeOutboxRow cycled = pendingRow(outbox.pendingBatch(1));
    CHECK(cycled.eventId != audited.eventId);
    CHECK(cycled.payload.find("porch") != std::string::npos);
    CHECK(outbox.markSent(cycled.eventId, 2500));

    ModuleAuditInput unchanged;
    unchanged.recordId = 7;
    unchanged.tableName = TableName::Camera;
    unchanged.before["id"] = 7;
    unchanged.after = unchanged.before;
    drogon::sync_wait(sink.publishAudit(unchanged));
    CHECK_FALSE(hasPending(outbox));

    SocketEmitDto oversized = addCamera(11, "loft");
    oversized.obj["config"] =
        std::string(NatsCameraChangeSink::kMaxPayloadBytes + 1, 'x');
    CHECK_THROWS_AS(
        drogon::sync_wait(sink.emitModule(
            {.table = TableName::Camera, .body = oversized, .client = nullptr})),
        ResponseException);
    CHECK_FALSE(hasPending(outbox));

    SocketEmitDto idless = addCamera(12, "yard");
    idless.obj.removeMember("id");
    CHECK_THROWS_AS(
        drogon::sync_wait(sink.emitModule(
            {.table = TableName::Camera, .body = idless, .client = nullptr})),
        ResponseException);
    CHECK_FALSE(hasPending(outbox));

    SocketEmitDto misnamed = addCamera(13, "hall");
    misnamed.obj["id"] = "13";
    CHECK_THROWS_AS(
        drogon::sync_wait(sink.emitModule(
            {.table = TableName::Camera, .body = misnamed, .client = nullptr})),
        ResponseException);
    CHECK_FALSE(hasPending(outbox));
  }

  {
    NatsCameraChangeSink sink(
        nullptr, NatsCameraChangeSink::Config{.retryMs = 20,
                                              .publishSubject = {},
                                              .streamName = {}});
    sink.reconcile();
    SocketEmitDto removal;
    removal.operation = SyncOperation::Delete;
    removal.option = TableName::Zone;
    removal.obj["id"] = static_cast<Json::Int64>(3);
    removal.obj["deletedAt"] = static_cast<Json::Int64>(4242);
    drogon::sync_wait(sink.emitModule(
        {.table = TableName::Zone, .body = removal, .client = nullptr}));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const ChangeOutboxRow waiting = pendingRow(outbox.pendingBatch(1));
    CHECK(waiting.eventId ==
          change_outbox_key::eventId(
              {.table = "zone",
               .recordId = 3,
               .discriminator = json_util::toString(removal.toJson())}));
    CHECK(waiting.payload == json_util::toString(removal.toJson()));
    CHECK(waiting.attempts == 0);
    CHECK_FALSE(sink.drained());
    sink.requestStop();
    CHECK(waitForDrain(sink, std::chrono::seconds(5)));
    sink.requestStop();
    CHECK(sink.drained());
    CHECK(outbox.markSent(waiting.eventId, 3000));
  }

  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url != nullptr && *url != '\0') {
    const std::string run = std::to_string(::getpid());
    const std::string stream = "argus-test-change-" + run;
    const std::string subject = "argus.test.change.flush." + run;
    auto liveBus = std::make_shared<NatsBus>();
    NatsBus::Options options;
    options.url = url;
    options.reconnectWaitMs = 200;
    options.maxReconnects = 5;
    REQUIRE(liveBus->connect(options));
    REQUIRE(liveBus->ensureStream({.name = stream,
                                   .subjects = {subject},
                                   .maxAgeNs = 3600000000000LL,
                                   .duplicatesNs = 120000000000LL}));

    std::mutex mutex;
    std::condition_variable cv;
    std::string received;
    const auto subscription = liveBus->subscribeDurable(
        {.stream = stream,
         .durable = "change-outbox-live-" + run,
         .subject = subject,
         .deliverAll = true,
         .maxDeliver = 3,
         .maxAckPending = NatsBus::kDefaultMaxAckPending,
         .handler = [&mutex, &cv, &received](
                        const NatsBus::DurableMessage& message,
                        const NatsBus::DurableSettlement& settlement) {
           {
             std::scoped_lock lock(mutex);
             received = std::string(message.payload);
           }
           settlement.ack();
           cv.notify_all();
         }});
    REQUIRE(subscription.has_value());

    const SocketEmitDto live = addCamera(99, "hall-" + run);
    const std::string expected = json_util::toString(live.toJson());
    {
      NatsCameraChangeSink liveSink(
          liveBus,
          NatsCameraChangeSink::Config{.retryMs = 20,
                                       .publishSubject = subject,
                                       .streamName = stream});
      liveSink.reconcile();
      drogon::sync_wait(liveSink.emitModule(
          {.table = TableName::Camera, .body = live, .client = nullptr}));

      {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(10),
                    [&received, &expected]() { return received == expected; });
      }
      for (int attempt = 0;
           attempt < 200 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));

      std::string seen;
      {
        std::scoped_lock lock(mutex);
        seen = received;
      }
      CHECK(seen == expected);
    }

    {
      NatsCameraChangeSink bursts(
          liveBus,
          NatsCameraChangeSink::Config{.retryMs = 20,
                                       .publishSubject = subject,
                                       .streamName = stream});
      const int64_t watermark = outboxWatermark();
      const auto started = std::chrono::steady_clock::now();
      for (int64_t recordId = 200; recordId < 300; ++recordId)
        drogon::sync_wait(bursts.emitModule(
            {.table = TableName::Camera,
             .body = addCamera(recordId, "hall"),
             .client = nullptr}));
      CHECK(rowsAfter(watermark) == 100);
      bursts.reconcile();
      for (int attempt = 0;
           attempt < 200 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      CHECK(sentRowsAfter(watermark) == 100);
      CHECK(std::chrono::steady_clock::now() - started <
            std::chrono::seconds(3));
    }

    {
      const std::string freshStream = "argus-test-heal-" + run;
      const std::string freshSubject = "argus.test.heal.change." + run;
      REQUIRE_FALSE(liveBus->streamInfo(freshStream).has_value());
      NatsCameraChangeSink healing(
          liveBus,
          NatsCameraChangeSink::Config{.retryMs = 20,
                                       .publishSubject = freshSubject,
                                       .streamName = freshStream});
      healing.reconcile();
      drogon::sync_wait(healing.emitModule(
          {.table = TableName::Camera,
           .body = addCamera(300, "gate"),
           .client = nullptr}));
      for (int attempt = 0; attempt < 200 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      CHECK(liveBus->streamInfo(freshStream).has_value());
    }

    NatsCameraChangeSink stranded(
        liveBus,
        NatsCameraChangeSink::Config{.retryMs = 20,
                                     .publishSubject = "argus.test.change.*",
                                     .streamName = stream});
    stranded.reconcile();
    const SocketEmitDto attic = addCamera(100, "attic");
    const SocketEmitDto cellar = addCamera(101, "cellar");
    drogon::sync_wait(stranded.emitModule(
        {.table = TableName::Camera, .body = attic, .client = nullptr}));
    drogon::sync_wait(stranded.emitModule(
        {.table = TableName::Camera, .body = cellar, .client = nullptr}));

    bool attempted = false;
    for (int attempt = 0; attempt < 100 && !attempted; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      const auto stuck = outbox.pendingBatch(1);
      attempted = !stuck.empty() && stuck.front().attempts > 0;
    }
    CHECK(attempted);
    CHECK(pendingRow(outbox.pendingBatch(1)).eventId ==
          change_outbox_key::eventId(
              {.table = "camera",
               .recordId = 100,
               .discriminator = json_util::toString(attic.toJson())}));
  }

  std::remove(kSinkDb);
  std::remove((std::string(kSinkDb) + "-wal").c_str());
  std::remove((std::string(kSinkDb) + "-shm").c_str());
}
