#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <productivity/nats-productivity-change-sink.hxx>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
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
    // Drogon reports the app running before its main loop is looping, and a
    // loop that has not begun cannot be stopped: trantor's loop() clears the
    // quit flag again as it starts. Waiting for it to loop is what makes the
    // quit below take effect — detaching in that window left the app's thread
    // running past the end of the process, measured as SIGSEGV inside
    // EventLoop::loop() in 3 of 20 runs of a forced constructor throw.
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    // A boot that never reached the loop at all is left to the process: it
    // cannot be asked to stop, and joining it would block for ever.
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

// The head pending row, reported as a failed assertion when the outbox holds none.
ChangeOutboxRow pendingRow(const std::vector<ChangeOutboxRow>& rows)
{
  REQUIRE(!rows.empty());
  return rows.empty() ? ChangeOutboxRow{} : rows.front();
}

// Whether the outbox holds anything, for the polls that watch a backlog drain.
bool hasPending(const ChangeOutboxRepository& outbox)
{
  return !outbox.pendingBatch(1).empty();
}

// The live stream, reported as a failed assertion when the broker holds none.
NatsBus::StreamStatus
streamStatus(const std::optional<NatsBus::StreamStatus>& status)
{
  REQUIRE(status.has_value());
  return status.value_or(NatsBus::StreamStatus{});
}

// One project row as the feature services hand it to the funnel: the
// SocketEmitDto triple with the row under `info`.
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

// One project row's status transition, for the audit leg.
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
} // namespace

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

    // The emit leg: the row event the fan-out routes into the recipients'
    // rooms, carrying the SocketEmitDto triple the wire contract freezes.
    const SocketEmitDto created = projectRow(SyncOperation::Add, 42, "Gate");
    drogon::sync_wait(sink.emitUsers({42, 7}, created));
    const ChangeOutboxRow emitted = pendingRow(outbox.pendingBatch(1));
    // The prefix plus 32 hex digits, inside the JetStream header budget.
    CHECK(emitted.eventId.rfind("productivity-change:", 0) == 0);
    CHECK(emitted.eventId.size() == 52);
    CHECK(emitted.payload.find("\"operation\":4") != std::string::npos);
    CHECK(emitted.payload.find("\"option\":\"project\"") != std::string::npos);
    CHECK(emitted.payload.find("\"info\":{") != std::string::npos);
    CHECK(emitted.payload.find("\"name\":\"Gate\"") != std::string::npos);
    CHECK(emitted.payload.find("\"users\":[42,7]") != std::string::npos);
    // An emit carries no `kind`: the fan-out reads a missing kind as a row
    // event and only "audit" as a diff.
    CHECK(emitted.payload.find("\"kind\"") == std::string::npos);
    CHECK(outbox.markSent(emitted.eventId, 1000));

    // The recipients travel exactly as the service named them: the emit leg is
    // the client's own row, and the funnel is not the place that rewrites it.
    drogon::sync_wait(sink.emitUsers({42, 42}, created));
    const ChangeOutboxRow verbatim = pendingRow(outbox.pendingBatch(1));
    CHECK(verbatim.eventId != emitted.eventId);
    CHECK(verbatim.payload.find("\"users\":[42,42]") != std::string::npos);
    CHECK(outbox.markSent(verbatim.eventId, 1100));

    // A redelivery of the same shape is a replay of the event already
    // published, not a second row: the id names the transition, so the same
    // transition cannot be published twice.
    drogon::sync_wait(sink.emitUsers({42, 7}, created));
    CHECK_FALSE(hasPending(outbox));

    // A transition is discriminated by its own payload, so the same record
    // moving again is its own event, while a redelivery of the same shape is a
    // replay of the row already published.
    drogon::sync_wait(
        sink.emitUsers({42, 7}, projectRow(SyncOperation::Add, 42, "Fence")));
    const ChangeOutboxRow moved = pendingRow(outbox.pendingBatch(1));
    CHECK(moved.eventId != emitted.eventId);
    CHECK(outbox.markSent(moved.eventId, 1200));

    // The delete emits a tombstone, and the emit addresses the member alone.
    SocketEmitDto tombstone;
    tombstone.operation = SyncOperation::Delete;
    tombstone.option = TableName::Project;
    tombstone.obj["id"] = static_cast<Json::Int64>(42);
    tombstone.obj["deletedAt"] = static_cast<Json::Int64>(1700000000);
    drogon::sync_wait(sink.emitUser(9, tombstone));
    const ChangeOutboxRow deleted = pendingRow(outbox.pendingBatch(1));
    CHECK(deleted.payload.find("\"operation\":5") != std::string::npos);
    CHECK(deleted.payload.find("\"deletedAt\":1700000000") !=
          std::string::npos);
    CHECK(deleted.payload.find("\"users\":[9]") != std::string::npos);
    CHECK(outbox.markSent(deleted.eventId, 1300));

    // The event id names the record the row moved on, so an emit that carries
    // none — or one that is not an id — has nothing to be keyed by and is not
    // recorded rather than being recorded under a wrong name.
    SocketEmitDto anonymous;
    anonymous.operation = SyncOperation::Add;
    anonymous.option = TableName::Project;
    anonymous.obj["name"] = "Gate";
    drogon::sync_wait(sink.emitUsers({42}, anonymous));
    CHECK_FALSE(hasPending(outbox));

    SocketEmitDto misnamed = anonymous;
    misnamed.obj["id"] = "42";
    drogon::sync_wait(sink.emitUsers({42}, misnamed));
    CHECK_FALSE(hasPending(outbox));

    // The audit leg: the per-field diff of a row that changed, addressed to
    // the users it moved for.
    drogon::sync_wait(sink.publishAudit(projectAudit(7, {42, 7})));
    const ChangeOutboxRow audited = pendingRow(outbox.pendingBatch(1));
    CHECK(audited.payload.find("\"kind\":\"audit\"") != std::string::npos);
    CHECK(audited.payload.find("\"table_name\":\"project\"") !=
          std::string::npos);
    CHECK(audited.payload.find("\"record_id\":7") != std::string::npos);
    CHECK(audited.payload.find("\"users\":[42,7]") != std::string::npos);
    // Both sides of the transition travel, so the client applies the diff
    // without the row it moved away from.
    CHECK(audited.payload.find("\"current\":\"paused\"") != std::string::npos);
    CHECK(audited.payload.find("\"previous\":\"active\"") != std::string::npos);
    CHECK(outbox.markSent(audited.eventId, 1400));

    // A payload past the broker's budget is refused rather than written: one
    // such row would stop every change queued behind it for ever.
    SocketEmitDto oversized = projectRow(SyncOperation::Add, 8, "Gate");
    oversized.obj["description"] =
        std::string(NatsProductivityChangeSink::kMaxPayloadBytes + 1, 'x');
    drogon::sync_wait(sink.emitUsers({42}, oversized));
    CHECK_FALSE(hasPending(outbox));

    // Recipients are deduplicated and a non-positive id is not a recipient,
    // so a diff with no one left to tell is not an event at all.
    drogon::sync_wait(sink.publishAudit(projectAudit(9, {42, 42, 0, -3})));
    const ChangeOutboxRow deduped = pendingRow(outbox.pendingBatch(1));
    CHECK(deduped.payload.find("\"users\":[42]") != std::string::npos);
    CHECK(deduped.payload.find("\"users\":[42,42]") == std::string::npos);
    CHECK(outbox.markSent(deduped.eventId, 1500));

    drogon::sync_wait(sink.publishAudit(projectAudit(10, {})));
    CHECK_FALSE(hasPending(outbox));

    // Nothing moved: an unchanged row has no diff and so no event.
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
    // No bus at all: the row waits in the outbox rather than being lost, and
    // the drain counts no attempt because the broker was never asked.
    NatsProductivityChangeSink sink(
        nullptr, NatsProductivityChangeSink::Config{
                     .retryMs = 20, .publishSubject = {}, .streamName = {}});
    sink.reconcile();
    drogon::sync_wait(
        sink.emitUsers({42}, projectRow(SyncOperation::Add, 12, "Gate")));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const ChangeOutboxRow waiting = pendingRow(outbox.pendingBatch(1));
    CHECK(waiting.eventId.rfind("productivity-change:", 0) == 0);
    CHECK(waiting.payload.find("\"id\":12") != std::string::npos);
    CHECK(waiting.attempts == 0);
    CHECK(outbox.markSent(waiting.eventId, 3000));
  }

  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url != nullptr && *url != '\0') {
    // A stream, a subject and a payload per run: a fixed event id republished
    // inside the stream's duplicate window is deduplicated, so a second run
    // would watch its own publish be accepted and deliver nothing.
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

    // A false mark-sent and a real publish both empty the outbox, so the leg
    // reads the event back off the broker: draining proves nothing on its own.
    // Delivering all closes the window between asking for the consumer and the
    // broker creating it, which a new-only consumer would leave open.
    std::mutex mutex;
    std::condition_variable cv;
    std::string received;
    const auto subscription = liveBus->subscribeDurable(
        {.stream = stream,
         .durable = "productivity-change-outbox-live-" + run,
         .subject = subject,
         .deliverAll = true,
         .maxDeliver = 3,
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
      // Scoped: a live sink left running would publish the rows of the block
      // below on its own subject, which is what that block measures.
      NatsProductivityChangeSink liveSink(
          liveBus, NatsProductivityChangeSink::Config{.retryMs = 20,
                                                      .publishSubject = subject,
                                                      .streamName = stream});
      // The row is read off the outbox before the drain starts: a reconciled
      // sink may have published and settled it by the time the test looks.
      drogon::sync_wait(
          liveSink.emitUsers({42, 7}, projectRow(SyncOperation::Add, 99,
                                                 "Gate")));
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

    // A publish is a JetStream publish, so the change subject needs a stream or
    // nothing is ever stored: a sink pointed at a stream that does not exist
    // yet owns creating it, and a row that settles is the proof it did.
    {
      const std::string healed = stream + "-healed";
      const std::string healedSubject = subject + ".healed";
      NatsProductivityChangeSink healer(
          liveBus, NatsProductivityChangeSink::Config{
                       .retryMs = 20,
                       .publishSubject = healedSubject,
                       .streamName = healed});
      healer.reconcile();
      drogon::sync_wait(
          healer.emitUsers({42}, projectRow(SyncOperation::Add, 102, "Gate")));
      for (int attempt = 0;
           attempt < 200 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      const auto info = streamStatus(liveBus->streamInfo(healed));
      CHECK(info.subjects.size() == 1);
      CHECK(info.subjects.front() == healedSubject);
    }

    // A backlog is one pass and not one tick per row: more changes than a single
    // batch holds all settle, and they are written before the drain starts so
    // that a drain fallen back to one row per pass cannot hide behind the wake
    // each enqueue gives it — 100 rows at a 50 ms tick is five seconds.
    {
      const std::string burstStream = stream + "-burst";
      const std::string burstSubject = subject + ".burst";
      NatsProductivityChangeSink bursts(
          liveBus, NatsProductivityChangeSink::Config{.retryMs = 20,
                                                      .publishSubject =
                                                          burstSubject,
                                                      .streamName = burstStream});
      const auto started = std::chrono::steady_clock::now();
      for (int64_t recordId = 200; recordId < 300; ++recordId)
        drogon::sync_wait(bursts.emitUsers(
            {42}, projectRow(SyncOperation::Add, recordId, "Gate")));
      bursts.reconcile();
      for (int attempt = 0;
           attempt < 400 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      CHECK(std::chrono::steady_clock::now() - started <
            std::chrono::seconds(3));
    }

    // A publish that cannot succeed leaves its row pending with the attempt
    // counted, and the change queued behind it waits instead of overtaking it:
    // a wildcard is not a publishable subject, so the broker is never asked.
    NatsProductivityChangeSink stranded(
        liveBus, NatsProductivityChangeSink::Config{
                     .retryMs = 20,
                     .publishSubject = "argus.test.productivity.change.*",
                     .streamName = stream});
    stranded.reconcile();
    drogon::sync_wait(
        stranded.emitUsers({42}, projectRow(SyncOperation::Add, 100, "Gate")));
    drogon::sync_wait(
        stranded.emitUsers({42}, projectRow(SyncOperation::Add, 101, "Gate")));

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
