#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <camera/nats-camera-change-sink.hxx>
#include <drogon/drogon.h>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
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

SocketEmitDto addCamera(int64_t id, const std::string& name)
{
  SocketEmitDto body;
  body.operation = SyncOperation::Add;
  body.option = TableName::Camera;
  body.obj["id"] = static_cast<Json::Int64>(id);
  body.obj["name"] = name;
  return body;
}
} // namespace

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
    drogon::sync_wait(sink.emitModule(TableName::Camera, add));

    const ChangeOutboxRow created = pendingRow(outbox.pendingBatch(1));
    CHECK(created.eventId ==
          change_outbox_key::eventId(
              {.table = "camera",
               .recordId = 7,
               .discriminator = json_util::toString(add.toJson())}));
    CHECK(created.payload == json_util::toString(add.toJson()));
    // The prefix plus 32 hex digits, inside the JetStream header budget.
    CHECK(created.eventId.size() == 46);

    // A replayed transition writes nothing, and so does an unchanged row.
    CHECK(outbox.markSent(created.eventId, 1000));
    drogon::sync_wait(sink.emitModule(TableName::Camera, add));
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
    // The id is the payload's own name, so a second audit of the same
    // before/after is a second event unless its timestamp matches as well.
    CHECK(audited.eventId.rfind("camera-change:", 0) == 0);
    CHECK(audited.eventId.size() == 46);
    CHECK(audited.payload.find("\"kind\":\"audit\"") != std::string::npos);
    CHECK(audited.payload.find("porch") != std::string::npos);
    CHECK(outbox.markSent(audited.eventId, 2000));

    // The a→b→a→b cycle: a record returns to a state it already held, so the
    // same diff is audited twice. Both are events and the payload names them,
    // not the state they moved away from, so the second lands as its own row.
    // The payload carries a millisecond timestamp, so the loop gives the clock
    // a millisecond to leave the first call's own reading.
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

    // A payload past the broker's budget is refused rather than written: one
    // such row would stop every change queued behind it for ever.
    SocketEmitDto oversized = addCamera(11, "loft");
    oversized.obj["config"] =
        std::string(NatsCameraChangeSink::kMaxPayloadBytes + 1, 'x');
    drogon::sync_wait(sink.emitModule(TableName::Camera, oversized));
    CHECK_FALSE(hasPending(outbox));
  }

  {
    // No bus at all: the row waits in the outbox rather than being lost.
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
    drogon::sync_wait(sink.emitModule(TableName::Zone, removal));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const ChangeOutboxRow waiting = pendingRow(outbox.pendingBatch(1));
    CHECK(waiting.eventId ==
          change_outbox_key::eventId(
              {.table = "zone",
               .recordId = 3,
               .discriminator = json_util::toString(removal.toJson())}));
    CHECK(waiting.payload == json_util::toString(removal.toJson()));
    CHECK(waiting.attempts == 0);
    CHECK(outbox.markSent(waiting.eventId, 3000));
  }

  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url != nullptr && *url != '\0') {
    // A stream, a subject and a payload per run: a fixed event id republished
    // inside the stream's duplicate window is deduplicated, so a second run
    // would watch its own publish be accepted and deliver nothing.
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

    // A false mark-sent and a real publish both empty the outbox, so the leg
    // reads the event back off the broker: draining proves nothing on its own.
    // Delivering all closes the window between asking for the consumer and the
    // broker creating it, which a new-only consumer would leave open.
    std::mutex mutex;
    std::condition_variable cv;
    std::string received;
    const auto subscription = liveBus->subscribeDurable(
        {.stream = stream,
         .durable = "change-outbox-live-" + run,
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

    const SocketEmitDto live = addCamera(99, "hall-" + run);
    const std::string expected = json_util::toString(live.toJson());
    {
      // Scoped: a live sink left running would publish the rows of the block
      // below on its own subject, which is what that block measures.
      NatsCameraChangeSink liveSink(
          liveBus,
          NatsCameraChangeSink::Config{.retryMs = 20,
                                       .publishSubject = subject,
                                       .streamName = stream});
      liveSink.reconcile();
      drogon::sync_wait(liveSink.emitModule(TableName::Camera, live));

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

    // A backlog is one pass and not one tick per row: more changes than a
    // single batch holds all settle, and they are written before the drain
    // starts so that a drain fallen back to one row per pass cannot hide
    // behind the wake each enqueue gives it -- 100 rows at a 50 ms tick is
    // five seconds.
    {
      NatsCameraChangeSink bursts(
          liveBus,
          NatsCameraChangeSink::Config{.retryMs = 20,
                                       .publishSubject = subject,
                                       .streamName = stream});
      const auto started = std::chrono::steady_clock::now();
      for (int64_t recordId = 200; recordId < 300; ++recordId)
        drogon::sync_wait(
            bursts.emitModule(TableName::Camera, addCamera(recordId, "hall")));
      bursts.reconcile();
      for (int attempt = 0;
           attempt < 200 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      CHECK(std::chrono::steady_clock::now() - started <
            std::chrono::seconds(3));
    }

    // The sink declares the stream it publishes into: a subject no stream
    // covers yet settles only because the sink reconciled one, which is the
    // state a broker-side wipe leaves the feed in.
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
      drogon::sync_wait(
          healing.emitModule(TableName::Camera, addCamera(300, "gate")));
      for (int attempt = 0; attempt < 200 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      CHECK(liveBus->streamInfo(freshStream).has_value());
    }

    // A publish that cannot succeed leaves its row pending with the attempt
    // counted, and the change queued behind it waits instead of overtaking it:
    // a wildcard is not a publishable subject, so the broker is never asked.
    NatsCameraChangeSink stranded(
        liveBus,
        NatsCameraChangeSink::Config{.retryMs = 20,
                                     .publishSubject = "argus.test.change.*",
                                     .streamName = stream});
    stranded.reconcile();
    const SocketEmitDto attic = addCamera(100, "attic");
    const SocketEmitDto cellar = addCamera(101, "cellar");
    drogon::sync_wait(stranded.emitModule(TableName::Camera, attic));
    drogon::sync_wait(stranded.emitModule(TableName::Camera, cellar));

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
