#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <notification/nats-notification-change-sink.hxx>
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

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kSinkDb = "notification-change-sink-test.db";

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

// One notification row's 0 -> 1 read transition, for the given recipients.
UserAuditInput readAudit(int64_t recordId, std::vector<int64_t> userIds)
{
  UserAuditInput input;
  input.recordId = recordId;
  input.tableName = TableName::Notification;
  input.before["id"] = static_cast<Json::Int64>(recordId);
  input.before["isRead"] = 0;
  input.after["id"] = static_cast<Json::Int64>(recordId);
  input.after["isRead"] = 1;
  input.userIds = std::move(userIds);
  return input;
}
} // namespace

TEST_CASE("the change sink lands every audit in the durable outbox")
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
  REQUIRE(DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA));

  ChangeOutboxRepository outbox;
  const UserAuditInput read = readAudit(7, {7});

  {
    NatsNotificationChangeSink sink(
        nullptr, NatsNotificationChangeSink::Config{
                     .retryMs = 20, .publishSubject = {}, .streamName = {}});
    drogon::sync_wait(sink.publishAudit(read));

    const ChangeOutboxRow marked = pendingRow(outbox.pendingBatch(1));
    // The prefix plus 32 hex digits, inside the JetStream header budget.
    CHECK(marked.eventId.rfind("notification-change:", 0) == 0);
    CHECK(marked.eventId.size() == 52);
    CHECK(marked.payload.find("\"kind\":\"audit\"") != std::string::npos);
    CHECK(marked.payload.find("\"table_name\":\"notification\"") !=
          std::string::npos);
    // The diff is per field and carries both sides of the transition, so the
    // client can apply it without the row it moved away from.
    CHECK(marked.payload.find("\"isRead\":{") != std::string::npos);
    CHECK(marked.payload.find("\"current\":1") != std::string::npos);
    CHECK(marked.payload.find("\"previous\":0") != std::string::npos);
    CHECK(marked.payload.find("\"users\":[7]") != std::string::npos);
    CHECK(marked.attempts == 0);
    CHECK(outbox.markSent(marked.eventId, 1000));

    // The id is the payload's own name, so a second audit of the same
    // before/after is a second event unless its timestamp matches as well.
    bool repeated = false;
    for (int attempt = 0; attempt < 50 && !repeated; ++attempt) {
      drogon::sync_wait(sink.publishAudit(read));
      repeated = hasPending(outbox);
      if (!repeated)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(repeated);
    const ChangeOutboxRow cycled = pendingRow(outbox.pendingBatch(1));
    CHECK(cycled.eventId != marked.eventId);
    CHECK(outbox.markSent(cycled.eventId, 2000));

    // Recipients are deduplicated and a non-positive id is not a recipient,
    // so a diff with no one left to tell is not an event at all.
    drogon::sync_wait(sink.publishAudit(readAudit(8, {7, 7, 0, -3})));
    const ChangeOutboxRow deduped = pendingRow(outbox.pendingBatch(1));
    CHECK(deduped.payload.find("\"users\":[7]") != std::string::npos);
    CHECK(deduped.payload.find("\"users\":[7,7]") == std::string::npos);
    CHECK(outbox.markSent(deduped.eventId, 2100));

    drogon::sync_wait(sink.publishAudit(readAudit(9, {})));
    CHECK_FALSE(hasPending(outbox));

    // Nothing moved: an unchanged row has no diff and so no event.
    UserAuditInput unchanged;
    unchanged.recordId = 10;
    unchanged.tableName = TableName::Notification;
    unchanged.before["id"] = 10;
    unchanged.after = unchanged.before;
    unchanged.userIds = {7};
    drogon::sync_wait(sink.publishAudit(unchanged));
    CHECK_FALSE(hasPending(outbox));

    // A payload past the broker's budget is refused rather than written: one
    // such row would stop every change queued behind it for ever.
    UserAuditInput oversized;
    oversized.recordId = 11;
    oversized.tableName = TableName::Notification;
    oversized.before["body"] = "";
    oversized.after["body"] =
        std::string(NatsNotificationChangeSink::kMaxPayloadBytes + 1, 'x');
    oversized.userIds = {7};
    drogon::sync_wait(sink.publishAudit(oversized));
    CHECK_FALSE(hasPending(outbox));
  }

  {
    // No bus at all: the row waits in the outbox rather than being lost.
    NatsNotificationChangeSink sink(
        nullptr, NatsNotificationChangeSink::Config{
                     .retryMs = 20, .publishSubject = {}, .streamName = {}});
    sink.reconcile();
    drogon::sync_wait(sink.publishAudit(readAudit(12, {7})));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const ChangeOutboxRow waiting = pendingRow(outbox.pendingBatch(1));
    CHECK(waiting.eventId.rfind("notification-change:", 0) == 0);
    CHECK(waiting.payload.find("\"record_id\":12") != std::string::npos);
    CHECK(waiting.attempts == 0);
    CHECK(outbox.markSent(waiting.eventId, 3000));
  }

  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url != nullptr && *url != '\0') {
    // A stream, a subject and a payload per run: a fixed event id republished
    // inside the stream's duplicate window is deduplicated, so a second run
    // would watch its own publish be accepted and deliver nothing.
    const std::string run = std::to_string(::getpid());
    const std::string stream = "argus-test-notification-change-" + run;
    const std::string subject = "argus.test.notification.change.flush." + run;
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
         .durable = "notification-change-outbox-live-" + run,
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
      NatsNotificationChangeSink liveSink(
          liveBus, NatsNotificationChangeSink::Config{.retryMs = 20,
                                                      .publishSubject = subject,
                                                      .streamName = stream});
      // The row is read off the outbox before the drain starts: a reconciled
      // sink may have published and settled it by the time the test looks.
      drogon::sync_wait(liveSink.publishAudit(readAudit(99, {7})));
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
      NatsNotificationChangeSink healer(
          liveBus, NatsNotificationChangeSink::Config{
                       .retryMs = 20,
                       .publishSubject = healedSubject,
                       .streamName = healed});
      healer.reconcile();
      drogon::sync_wait(healer.publishAudit(readAudit(102, {7})));
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
      NatsNotificationChangeSink bursts(
          liveBus, NatsNotificationChangeSink::Config{.retryMs = 20,
                                                      .publishSubject =
                                                          burstSubject,
                                                      .streamName = burstStream});
      const auto started = std::chrono::steady_clock::now();
      for (int64_t recordId = 200; recordId < 300; ++recordId)
        drogon::sync_wait(bursts.publishAudit(readAudit(recordId, {7})));
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
    NatsNotificationChangeSink stranded(
        liveBus, NatsNotificationChangeSink::Config{
                     .retryMs = 20,
                     .publishSubject = "argus.test.notification.change.*",
                     .streamName = stream});
    stranded.reconcile();
    drogon::sync_wait(stranded.publishAudit(readAudit(100, {7})));
    drogon::sync_wait(stranded.publishAudit(readAudit(101, {7})));

    bool attempted = false;
    for (int attempt = 0; attempt < 100 && !attempted; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      const auto stuck = outbox.pendingBatch(1);
      attempted = !stuck.empty() && stuck.front().attempts > 0;
    }
    CHECK(attempted);
    CHECK(pendingRow(outbox.pendingBatch(1)).payload.find("\"record_id\":100") !=
          std::string::npos);
  }

  std::remove(kSinkDb);
  std::remove((std::string(kSinkDb) + "-wal").c_str());
  std::remove((std::string(kSinkDb) + "-shm").c_str());
}
