#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <shared/services/change-sink/nats-productivity-change-sink.hxx>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <shared/repositories/change-outbox/change-outbox-status.hxx>
#include <sqlite/db-service.hxx>
#include <nats/nats-bus.hxx>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef ARGUS_PRODUCTIVITY_SCHEMA
#error "ARGUS_PRODUCTIVITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kSinkDb = "productivity-change-sink-test.db";

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

bool waitForDrain(const NatsProductivityChangeSink& sink,
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

NatsBus::StreamStatus
streamStatus(const std::optional<NatsBus::StreamStatus>& status)
{
  REQUIRE(status.has_value());
  return status.value_or(NatsBus::StreamStatus{});
}

SocketEmitDto projectRow(SyncOperation operation, int64_t recordId,
                         const std::string& name)
{
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::Project;
  body.obj["id"] = static_cast<Json::Int64>(recordId);
  body.obj["name"] = name;
  return body;
}

UserAuditInput projectAudit(int64_t recordId, std::vector<int64_t> userIds)
{
  UserAuditInput input;
  input.recordId = recordId;
  input.tableName = TableName::Project;
  input.before["id"] = static_cast<Json::Int64>(recordId);
  input.before["status"] = "active";
  input.after["id"] = static_cast<Json::Int64>(recordId);
  input.after["status"] = "paused";
  input.userIds = std::move(userIds);
  return input;
}
}

TEST_CASE("the change sink lands every emit and audit in the durable outbox")
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
  REQUIRE(DbService::runScriptFile(ARGUS_PRODUCTIVITY_SCHEMA));

  ChangeOutboxRepository outbox;

  {
    NatsProductivityChangeSink sink(
        nullptr, NatsProductivityChangeSink::Config{
                     .retryMs = 20, .publishSubject = {}, .streamName = {}});

    const SocketEmitDto created = projectRow(SyncOperation::Add, 42, "Gate");
    drogon::sync_wait(sink.emitUsers({.userIds = {42, 7}, .body = created}));
    const ChangeOutboxRow emitted = pendingRow(outbox.pendingBatch(1));
    CHECK(emitted.eventId.rfind("productivity-change:", 0) == 0);
    CHECK(emitted.eventId.size() == 52);
    CHECK(emitted.payload.find("\"operation\":4") != std::string::npos);
    CHECK(emitted.payload.find("\"option\":\"project\"") != std::string::npos);
    CHECK(emitted.payload.find("\"info\":{") != std::string::npos);
    CHECK(emitted.payload.find("\"name\":\"Gate\"") != std::string::npos);
    CHECK(emitted.payload.find("\"users\":[42,7]") != std::string::npos);
    CHECK(emitted.payload.find("\"kind\"") == std::string::npos);
    CHECK(outbox.markSent(emitted.eventId, 1000));

    drogon::sync_wait(sink.emitUsers({.userIds = {42, 42}, .body = created}));
    const ChangeOutboxRow verbatim = pendingRow(outbox.pendingBatch(1));
    CHECK(verbatim.eventId != emitted.eventId);
    CHECK(verbatim.payload.find("\"users\":[42,42]") != std::string::npos);
    CHECK(outbox.markSent(verbatim.eventId, 1100));

    drogon::sync_wait(sink.emitUsers({.userIds = {42, 7}, .body = created}));
    CHECK_FALSE(hasPending(outbox));

    drogon::sync_wait(
        sink.emitUsers({.userIds = {42, 7},
                    .body = projectRow(SyncOperation::Add, 42, "Fence")}));
    const ChangeOutboxRow moved = pendingRow(outbox.pendingBatch(1));
    CHECK(moved.eventId != emitted.eventId);
    CHECK(outbox.markSent(moved.eventId, 1200));

    SocketEmitDto tombstone;
    tombstone.operation = SyncOperation::Delete;
    tombstone.option = TableName::Project;
    tombstone.obj["id"] = static_cast<Json::Int64>(42);
    tombstone.obj["deletedAt"] = static_cast<Json::Int64>(1700000000);
    drogon::sync_wait(sink.emitUsers({.userIds = {9}, .body = tombstone}));
    const ChangeOutboxRow deleted = pendingRow(outbox.pendingBatch(1));
    CHECK(deleted.payload.find("\"operation\":5") != std::string::npos);
    CHECK(deleted.payload.find("\"deletedAt\":1700000000") !=
          std::string::npos);
    CHECK(deleted.payload.find("\"users\":[9]") != std::string::npos);
    CHECK(outbox.markSent(deleted.eventId, 1300));

    SocketEmitDto anonymous;
    anonymous.operation = SyncOperation::Add;
    anonymous.option = TableName::Project;
    anonymous.obj["name"] = "Gate";
    CHECK_THROWS_AS(
        drogon::sync_wait(sink.emitUsers({.userIds = {42}, .body = anonymous})),
        ResponseException);
    CHECK_FALSE(hasPending(outbox));

    SocketEmitDto misnamed = anonymous;
    misnamed.obj["id"] = "42";
    CHECK_THROWS_AS(
        drogon::sync_wait(sink.emitUsers({.userIds = {42}, .body = misnamed})),
        ResponseException);
    CHECK_FALSE(hasPending(outbox));

    drogon::sync_wait(sink.publishAudit(projectAudit(7, {42, 7})));
    const ChangeOutboxRow audited = pendingRow(outbox.pendingBatch(1));
    CHECK(audited.payload.find("\"kind\":\"audit\"") != std::string::npos);
    CHECK(audited.payload.find("\"table_name\":\"project\"") !=
          std::string::npos);
    CHECK(audited.payload.find("\"record_id\":7") != std::string::npos);
    CHECK(audited.payload.find("\"users\":[42,7]") != std::string::npos);
    CHECK(audited.payload.find("\"current\":\"paused\"") != std::string::npos);
    CHECK(audited.payload.find("\"previous\":\"active\"") != std::string::npos);
    CHECK(outbox.markSent(audited.eventId, 1400));

    SocketEmitDto oversized = projectRow(SyncOperation::Add, 8, "Gate");
    oversized.obj["description"] =
        std::string(NatsProductivityChangeSink::kMaxPayloadBytes + 1, 'x');
    CHECK_THROWS_AS(
        drogon::sync_wait(sink.emitUsers({.userIds = {42}, .body = oversized})),
        ResponseException);
    CHECK_FALSE(hasPending(outbox));

    drogon::sync_wait(sink.publishAudit(projectAudit(9, {42, 42, 0, -3})));
    const ChangeOutboxRow deduped = pendingRow(outbox.pendingBatch(1));
    CHECK(deduped.payload.find("\"users\":[42]") != std::string::npos);
    CHECK(deduped.payload.find("\"users\":[42,42]") == std::string::npos);
    CHECK(outbox.markSent(deduped.eventId, 1500));

    drogon::sync_wait(sink.publishAudit(projectAudit(10, {})));
    CHECK_FALSE(hasPending(outbox));

    UserAuditInput unchanged;
    unchanged.recordId = 11;
    unchanged.tableName = TableName::Project;
    unchanged.before["id"] = 11;
    unchanged.after = unchanged.before;
    unchanged.userIds = {42};
    drogon::sync_wait(sink.publishAudit(unchanged));
    CHECK_FALSE(hasPending(outbox));
  }

  {
    NatsProductivityChangeSink sink(
        nullptr, NatsProductivityChangeSink::Config{
                     .retryMs = 20, .publishSubject = {}, .streamName = {}});
    sink.reconcile();
    drogon::sync_wait(
        sink.emitUsers({.userIds = {42},
                        .body = projectRow(SyncOperation::Add, 12, "Gate")}));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const ChangeOutboxRow waiting = pendingRow(outbox.pendingBatch(1));
    CHECK(waiting.eventId.rfind("productivity-change:", 0) == 0);
    CHECK(waiting.payload.find("\"id\":12") != std::string::npos);
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
    const std::string stream = "argus-test-productivity-change-" + run;
    const std::string subject = "argus.test.productivity.change.flush." + run;
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
         .durable = "productivity-change-outbox-live-" + run,
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

    {
      NatsProductivityChangeSink liveSink(
          liveBus, NatsProductivityChangeSink::Config{.retryMs = 20,
                                                      .publishSubject = subject,
                                                      .streamName = stream});
      drogon::sync_wait(
          liveSink.emitUsers({.userIds = {42, 7},
                              .body = projectRow(SyncOperation::Add, 99,
                                                 "Gate")}));
      const std::string expected = pendingRow(outbox.pendingBatch(1)).payload;
      liveSink.reconcile();

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
      const std::string healed = stream + "-healed";
      const std::string healedSubject = subject + ".healed";
      NatsProductivityChangeSink healer(
          liveBus, NatsProductivityChangeSink::Config{
                       .retryMs = 20,
                       .publishSubject = healedSubject,
                       .streamName = healed});
      healer.reconcile();
      drogon::sync_wait(healer.emitUsers(
          {.userIds = {42},
           .body = projectRow(SyncOperation::Add, 102, "Gate")}));
      for (int attempt = 0;
           attempt < 200 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      const auto info = streamStatus(liveBus->streamInfo(healed));
      CHECK(info.subjects.size() == 1);
      CHECK(info.subjects.front() == healedSubject);
    }

    {
      const std::string eager = stream + "-eager";
      NatsProductivityChangeSink eagerSink(
          liveBus, NatsProductivityChangeSink::Config{
                       .retryMs = 20,
                       .publishSubject = subject + ".eager",
                       .streamName = eager});
      eagerSink.reconcile();
      bool created = false;
      for (int attempt = 0; attempt < 200 && !created; ++attempt) {
        created = liveBus->streamInfo(eager).has_value();
        if (!created)
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      CHECK(created);
      CHECK_FALSE(hasPending(outbox));
    }

    {
      const std::string burstStream = stream + "-burst";
      const std::string burstSubject = subject + ".burst";
      NatsProductivityChangeSink bursts(
          liveBus, NatsProductivityChangeSink::Config{.retryMs = 20,
                                                      .publishSubject =
                                                          burstSubject,
                                                      .streamName = burstStream});
      const int64_t watermark = outboxWatermark();
      const auto started = std::chrono::steady_clock::now();
      for (int64_t recordId = 200; recordId < 300; ++recordId)
        drogon::sync_wait(bursts.emitUsers(
            {.userIds = {42},
             .body = projectRow(SyncOperation::Add, recordId, "Gate")}));
      CHECK(rowsAfter(watermark) == 100);
      bursts.reconcile();
      for (int attempt = 0;
           attempt < 400 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      CHECK(sentRowsAfter(watermark) == 100);
      CHECK(std::chrono::steady_clock::now() - started <
            std::chrono::seconds(3));
    }

    NatsProductivityChangeSink stranded(
        liveBus, NatsProductivityChangeSink::Config{
                     .retryMs = 20,
                     .publishSubject = "argus.test.productivity.change.*",
                     .streamName = stream});
    stranded.reconcile();
    drogon::sync_wait(stranded.emitUsers(
        {.userIds = {42},
         .body = projectRow(SyncOperation::Add, 100, "Gate")}));
    drogon::sync_wait(stranded.emitUsers(
        {.userIds = {42},
         .body = projectRow(SyncOperation::Add, 101, "Gate")}));

    bool attempted = false;
    for (int attempt = 0; attempt < 100 && !attempted; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      const auto stuck = outbox.pendingBatch(1);
      attempted = !stuck.empty() && stuck.front().attempts > 0;
    }
    CHECK(attempted);
    CHECK(pendingRow(outbox.pendingBatch(1)).payload.find("\"id\":100") !=
          std::string::npos);
  }

  std::remove(kSinkDb);
  std::remove((std::string(kSinkDb) + "-wal").c_str());
  std::remove((std::string(kSinkDb) + "-shm").c_str());
}
