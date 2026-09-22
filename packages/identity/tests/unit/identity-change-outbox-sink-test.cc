#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/api/user/services/nats-identity-change-sink.hxx>
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

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kSinkDb = "identity-change-sink-test.db";
constexpr const char* kChangeSubject = "argus.identity.v1.change";
constexpr const char* kActionSubject = "argus.identity.v1.user-action";

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
    // quit below take effect -- detaching in that window left the app's thread
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

// The head pending row, reported as a failed assertion when the outbox holds
// none.
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

// One user row as the auth and user feature services hand it to the catalog
// leg: the snapshot the memory replicas follow.
IdentityCatalogInput userCatalog(int64_t recordId, const std::string& name,
                                 bool deleted = false)
{
  IdentityCatalogInput input;
  input.table = TableName::User;
  input.id = recordId;
  input.deleted = deleted;
  input.row["id"] = static_cast<Json::Int64>(recordId);
  input.row["name"] = name;
  return input;
}

// One module emit as identity-rpc hands it over: the SocketEmitDto triple the
// wire contract freezes.
SocketEmitDto userRow(SyncOperation operation, int64_t recordId,
                      const std::string& name)
{
  SocketEmitDto body;
  body.operation = operation;
  body.option = TableName::User;
  body.obj["id"] = static_cast<Json::Int64>(recordId);
  body.obj["name"] = name;
  return body;
}

// One user row's role transition, for the user audit leg.
UserAuditInput userAudit(int64_t recordId, std::vector<int64_t> userIds)
{
  UserAuditInput input;
  input.recordId = recordId;
  input.tableName = TableName::User;
  input.before["id"] = static_cast<Json::Int64>(recordId);
  input.before["role"] = "guest";
  input.after["id"] = static_cast<Json::Int64>(recordId);
  input.after["role"] = "resident";
  input.userIds = std::move(userIds);
  return input;
}

// One invitation row's status transition, for the module audit leg.
ModuleAuditInput invitationAudit(int64_t recordId)
{
  ModuleAuditInput input;
  input.recordId = recordId;
  input.tableName = TableName::UserInvitation;
  input.before["id"] = static_cast<Json::Int64>(recordId);
  input.before["status"] = "pending";
  input.after["id"] = static_cast<Json::Int64>(recordId);
  input.after["status"] = "redeemed";
  input.actorId = 7;
  return input;
}

// One portrait view as the preview service journals it: the read is of a user
// row, and what the row carries is the fact of the view rather than a snapshot,
// because no column moved.
UserActionEvent portraitRead(int64_t portraitUserId)
{
  Json::Value viewed;
  viewed["event"] = "portrait_preview";
  viewed["portraitUserId"] = static_cast<Json::Int64>(portraitUserId);
  UserActionEvent event;
  event.userId = 7;
  event.recordId = portraitUserId;
  event.tableName = TableName::User;
  event.action = UserAction::Read;
  event.newData = viewed;
  event.ipAddress = "";
  return event;
}
} // namespace

TEST_CASE("the change sink lands every catalog row, emit, audit and journal "
          "row in the durable outbox")
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
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));

  ChangeOutboxRepository outbox;

  {
    NatsIdentityChangeSink sink(nullptr, NatsIdentityChangeSink::Config{});

    // The catalog leg: the post-write snapshot the memory replicas follow.
    drogon::sync_wait(sink.publishCatalog(userCatalog(42, "Ana")));
    const ChangeOutboxRow catalog = pendingRow(outbox.pendingBatch(1));
    // The prefix plus 32 hex digits, inside the JetStream header budget.
    CHECK(catalog.eventId.rfind("identity-change:", 0) == 0);
    CHECK(catalog.eventId.size() == 48);
    CHECK(catalog.subject == kChangeSubject);
    CHECK(catalog.payload.find("\"kind\":\"identity\"") != std::string::npos);
    CHECK(catalog.payload.find("\"table\":\"user\"") != std::string::npos);
    CHECK(catalog.payload.find("\"id\":42") != std::string::npos);
    CHECK(catalog.payload.find("\"deleted\":false") != std::string::npos);
    CHECK(catalog.payload.find("\"name\":\"Ana\"") != std::string::npos);
    CHECK(outbox.markSent(catalog.id, 1000));

    // A soft delete is the same leg with `deleted` set: the replica tombstones
    // the row it already holds.
    drogon::sync_wait(sink.publishCatalog(userCatalog(42, "Ana", true)));
    const ChangeOutboxRow deleted = pendingRow(outbox.pendingBatch(1));
    CHECK(deleted.payload.find("\"deleted\":true") != std::string::npos);

    // A redelivery of the same snapshot is a replay of the event already
    // published, not a second row: the id names the transition, so the same
    // transition cannot be published twice.
    CHECK(outbox.markSent(deleted.id, 1100));
    drogon::sync_wait(sink.publishCatalog(userCatalog(42, "Ana", true)));
    CHECK_FALSE(hasPending(outbox));

    // A transition is discriminated by its own payload, so the same record
    // moving to a new snapshot is its own event.
    drogon::sync_wait(sink.publishCatalog(userCatalog(42, "Ana Maria")));
    const ChangeOutboxRow moved = pendingRow(outbox.pendingBatch(1));
    CHECK(moved.eventId != catalog.eventId);
    CHECK(outbox.markSent(moved.id, 1200));

    // The module emit: the row event the transport routes into the module's
    // room, carrying the SocketEmitDto triple and the room the emit names.
    drogon::sync_wait(
        sink.emitModule(TableName::User, userRow(SyncOperation::Add, 42, "Ana")));
    const ChangeOutboxRow emitted = pendingRow(outbox.pendingBatch(1));
    CHECK(emitted.eventId.rfind("identity-change:", 0) == 0);
    CHECK(emitted.payload.find("\"operation\":4") != std::string::npos);
    CHECK(emitted.payload.find("\"option\":\"user\"") != std::string::npos);
    CHECK(emitted.payload.find("\"info\":{") != std::string::npos);
    CHECK(emitted.payload.find("\"name\":\"Ana\"") != std::string::npos);
    // An emit carries no `kind` and no recipients: the fan-out reads a missing
    // kind as a row event, and identity emits to a module room, never per user.
    CHECK(emitted.payload.find("\"kind\"") == std::string::npos);
    CHECK(emitted.payload.find("\"users\"") == std::string::npos);
    CHECK(outbox.markSent(emitted.id, 1300));

    // The invitation emit is the same leg under the table the invitation
    // feature names, so the frame's option follows the argument.
    drogon::sync_wait(sink.emitModule(TableName::UserInvitation,
                                     userRow(SyncOperation::Add, 9, "Ana")));
    const ChangeOutboxRow invitation = pendingRow(outbox.pendingBatch(1));
    CHECK(invitation.payload.find("\"option\":\"user_invitation\"") !=
          std::string::npos);
    CHECK(outbox.markSent(invitation.id, 1400));

    // A delete emits a tombstone: the operation is the delete and the row
    // carries only what the client needs to drop it.
    SocketEmitDto tombstone;
    tombstone.operation = SyncOperation::Delete;
    tombstone.option = TableName::User;
    tombstone.obj["id"] = static_cast<Json::Int64>(42);
    tombstone.obj["deletedAt"] = static_cast<Json::Int64>(1700000000);
    drogon::sync_wait(sink.emitModule(TableName::User, tombstone));
    const ChangeOutboxRow removed = pendingRow(outbox.pendingBatch(1));
    CHECK(removed.payload.find("\"operation\":5") != std::string::npos);
    CHECK(removed.payload.find("\"deletedAt\":1700000000") !=
          std::string::npos);
    CHECK(outbox.markSent(removed.id, 1500));

    // The event id names the record the row moved on, so an emit that carries
    // none -- or one that is not an id -- has nothing to be keyed by and is not
    // recorded rather than being recorded under a wrong name.
    SocketEmitDto anonymous;
    anonymous.operation = SyncOperation::Add;
    anonymous.option = TableName::User;
    anonymous.obj["name"] = "Ana";
    drogon::sync_wait(sink.emitModule(TableName::User, anonymous));
    CHECK_FALSE(hasPending(outbox));

    SocketEmitDto misnamed = anonymous;
    misnamed.obj["id"] = "42";
    drogon::sync_wait(sink.emitModule(TableName::User, misnamed));
    CHECK_FALSE(hasPending(outbox));

    // The module audit leg: the per-field diff of a row that changed, on the
    // change subject, with the actor that moved it.
    drogon::sync_wait(sink.publishModuleAudit(invitationAudit(9)));
    const ChangeOutboxRow audited = pendingRow(outbox.pendingBatch(1));
    CHECK(audited.subject == kChangeSubject);
    CHECK(audited.payload.find("\"kind\":\"audit\"") != std::string::npos);
    CHECK(audited.payload.find("\"table_name\":\"user_invitation\"") !=
          std::string::npos);
    CHECK(audited.payload.find("\"record_id\":9") != std::string::npos);
    CHECK(audited.payload.find("\"create_user_id\":7") != std::string::npos);
    // Both sides of the transition travel, and never the invitation token: the
    // audit carries the diff of the columns it was handed, nothing else.
    CHECK(audited.payload.find("\"current\":\"redeemed\"") != std::string::npos);
    CHECK(audited.payload.find("\"previous\":\"pending\"") != std::string::npos);
    CHECK(outbox.markSent(audited.id, 1600));

    // Nothing moved: an unchanged row has no diff and so no event.
    ModuleAuditInput unchanged;
    unchanged.recordId = 10;
    unchanged.tableName = TableName::UserInvitation;
    unchanged.before["id"] = 10;
    unchanged.after = unchanged.before;
    drogon::sync_wait(sink.publishModuleAudit(unchanged));
    CHECK_FALSE(hasPending(outbox));

    // The user audit leg: the same diff, addressed to the users it moved for.
    drogon::sync_wait(sink.publishUsersAudit(userAudit(42, {42, 7})));
    const ChangeOutboxRow perUser = pendingRow(outbox.pendingBatch(1));
    CHECK(perUser.payload.find("\"kind\":\"audit\"") != std::string::npos);
    CHECK(perUser.payload.find("\"table_name\":\"user\"") !=
          std::string::npos);
    CHECK(perUser.payload.find("\"users\":[42,7]") != std::string::npos);
    CHECK(perUser.payload.find("\"current\":\"resident\"") !=
          std::string::npos);
    CHECK(outbox.markSent(perUser.id, 1700));

    // Recipients are deduplicated and a non-positive id is not a recipient, so
    // a diff with no one left to tell is not an event at all.
    drogon::sync_wait(sink.publishUsersAudit(userAudit(43, {42, 42, 0, -3})));
    const ChangeOutboxRow deduped = pendingRow(outbox.pendingBatch(1));
    CHECK(deduped.payload.find("\"users\":[42]") != std::string::npos);
    CHECK(deduped.payload.find("\"users\":[42,42]") == std::string::npos);
    CHECK(outbox.markSent(deduped.id, 1800));

    drogon::sync_wait(sink.publishUsersAudit(userAudit(44, {})));
    CHECK_FALSE(hasPending(outbox));

    // The journal leg: the actor's own record of one thing done to one row, on
    // the action subject rather than the change subject, because the journal is
    // not a change feed and its consumer is its own.
    drogon::sync_wait(sink.publishAction(portraitRead(42)));
    const ChangeOutboxRow journal = pendingRow(outbox.pendingBatch(1));
    CHECK(journal.subject == kActionSubject);
    CHECK(journal.subject != kChangeSubject);
    // No transition to be keyed by: this row's id is its identity.
    CHECK(journal.eventId.empty());
    CHECK(journal.payload.find("\"user_id\":7") != std::string::npos);
    CHECK(journal.payload.find("\"record_id\":42") != std::string::npos);
    CHECK(journal.payload.find("\"table_name\":\"user\"") != std::string::npos);
    CHECK(journal.payload.find("\"action\":\"read\"") != std::string::npos);
    // Every call site in this package journals without an address; the key
    // travels anyway, because the wire fixes it.
    CHECK(journal.payload.find("\"ip_address\":\"\"") != std::string::npos);
    CHECK(journal.payload.find("\"event\":\"portrait_preview\"") !=
          std::string::npos);
    CHECK(journal.payload.find("\"kind\"") == std::string::npos);
    // The derived id is what the drain publishes this row under, and two rows
    // never share one.
    CHECK(change_outbox_key::actionMsgId(journal.id) ==
          "identity-action:" + std::to_string(journal.id));
    CHECK(outbox.markSent(journal.id, 1900));

    // A second portrait view of the same record is a second row: a read changes
    // no row, so two reads are two audit rows and neither may collapse into the
    // other. This is the whole reason the journal is addressed by its own id.
    drogon::sync_wait(sink.publishAction(portraitRead(42)));
    const ChangeOutboxRow secondRead = pendingRow(outbox.pendingBatch(1));
    CHECK(secondRead.eventId.empty());
    CHECK(secondRead.id != journal.id);
    CHECK(secondRead.payload == journal.payload);
    CHECK(change_outbox_key::actionMsgId(secondRead.id) !=
          change_outbox_key::actionMsgId(journal.id));
    CHECK(outbox.markSent(secondRead.id, 2000));

    // A payload past the broker's budget is refused rather than written, on
    // both legs: one such row would stop every change queued behind it for
    // ever.
    IdentityCatalogInput oversized = userCatalog(8, "Ana");
    oversized.row["portrait"] =
        std::string(NatsIdentityChangeSink::kMaxPayloadBytes + 1, 'x');
    drogon::sync_wait(sink.publishCatalog(oversized));
    CHECK_FALSE(hasPending(outbox));

    UserActionEvent oversizedAction = portraitRead(9);
    oversizedAction.newData["portrait"] =
        std::string(NatsIdentityChangeSink::kMaxPayloadBytes + 1, 'x');
    drogon::sync_wait(sink.publishAction(oversizedAction));
    CHECK_FALSE(hasPending(outbox));
  }

  {
    // No bus at all: both subjects' rows wait in the outbox rather than being
    // lost, and the drain counts no attempt because the broker was never asked.
    NatsIdentityChangeSink sink(nullptr, NatsIdentityChangeSink::Config{});
    sink.reconcile();
    drogon::sync_wait(sink.publishCatalog(userCatalog(12, "Ana")));
    drogon::sync_wait(sink.publishAction(portraitRead(12)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const ChangeOutboxRow change = pendingRow(outbox.pendingBatch(1));
    CHECK(change.subject == kChangeSubject);
    CHECK(change.payload.find("\"id\":12") != std::string::npos);
    CHECK(change.attempts == 0);
    CHECK(outbox.markSent(change.id, 3000));

    const ChangeOutboxRow action = pendingRow(outbox.pendingBatch(1));
    CHECK(action.subject == kActionSubject);
    CHECK(action.eventId.empty());
    CHECK(action.attempts == 0);
    CHECK(outbox.markSent(action.id, 3100));
    CHECK_FALSE(hasPending(outbox));
  }

  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url != nullptr && *url != '\0') {
    // A stream, two subjects and a payload per run: a fixed event id
    // republished inside the stream's duplicate window is deduplicated, so a
    // second run would watch its own publish be accepted and deliver nothing.
    const std::string run = std::to_string(::getpid());
    const std::string stream = "argus-test-identity-change-" + run;
    const std::string changeSubject = "argus.test.identity.change." + run;
    const std::string actionSubject = "argus.test.identity.action." + run;
    auto liveBus = std::make_shared<NatsBus>();
    NatsBus::Options options;
    options.url = url;
    options.reconnectWaitMs = 200;
    options.maxReconnects = 5;
    REQUIRE(liveBus->connect(options));
    REQUIRE(liveBus->ensureStream({.name = stream,
                                   .subjects = {changeSubject, actionSubject},
                                   .maxAgeNs = 3600000000000LL,
                                   .duplicatesNs = 120000000000LL}));

    // A false mark-sent and a real publish both empty the outbox, so each leg
    // reads its event back off the broker: draining proves nothing on its own.
    // Delivering all closes the window between asking for the consumer and the
    // broker creating it, which a new-only consumer would leave open.
    std::mutex mutex;
    std::condition_variable cv;
    std::string changed;
    const auto changeSubscription = liveBus->subscribeDurable(
        {.stream = stream,
         .durable = "identity-change-outbox-live-" + run,
         .subject = changeSubject,
         .deliverAll = true,
         .maxDeliver = 3,
         .handler = [&mutex, &cv, &changed](
                        const NatsBus::DurableMessage& message,
                        const NatsBus::DurableSettlement& settlement) {
           {
             std::scoped_lock lock(mutex);
             changed = std::string(message.payload);
           }
           settlement.ack();
           cv.notify_all();
         }});
    REQUIRE(changeSubscription.has_value());

    std::size_t journalDeliveries = 0;
    std::vector<std::string> journalPayloads;
    const auto actionSubscription = liveBus->subscribeDurable(
        {.stream = stream,
         .durable = "identity-action-journal-live-" + run,
         .subject = actionSubject,
         .deliverAll = true,
         .maxDeliver = 3,
         .handler = [&mutex, &cv, &journalDeliveries, &journalPayloads](
                        const NatsBus::DurableMessage& message,
                        const NatsBus::DurableSettlement& settlement) {
           {
             std::scoped_lock lock(mutex);
             ++journalDeliveries;
             journalPayloads.emplace_back(message.payload);
           }
           settlement.ack();
           cv.notify_all();
         }});
    REQUIRE(actionSubscription.has_value());

    // Scoped: a live sink left running would publish the rows of the blocks
    // below on its own subjects, which is what those blocks measure.
    {
      // Both legs drain through one sink for the whole block: a second sink
      // would race this one for the same pending rows.
      NatsIdentityChangeSink liveSink(
          liveBus,
          NatsIdentityChangeSink::Config{.retryMs = 20,
                                         .changeSubject = changeSubject,
                                         .actionSubject = actionSubject,
                                         .streamName = stream});
      // The catalog row is read off the outbox before the drain starts: a
      // reconciled sink may have published and settled it by the time the test
      // looks.
      drogon::sync_wait(liveSink.publishCatalog(userCatalog(99, "Ana")));
      const std::string expected = pendingRow(outbox.pendingBatch(1)).payload;
      liveSink.reconcile();

      {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(10),
                    [&changed, &expected]() { return changed == expected; });
      }
      for (int attempt = 0; attempt < 200 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      std::string seen;
      {
        std::scoped_lock lock(mutex);
        seen = changed;
      }
      CHECK(seen == expected);

      // The journal leg over the broker: two views of one portrait, byte for
      // byte the same payload. Both are stored and both are delivered, because
      // each is published under its own row's id -- the reading that would key
      // them by content would have the second dropped as a duplicate of the
      // first, which is exactly the audit row nobody would notice missing.
      for (int i = 0; i < 2; ++i)
        drogon::sync_wait(liveSink.publishAction(portraitRead(99)));
      {
        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(10),
                    [&journalDeliveries]() { return journalDeliveries >= 2; });
      }
      for (int attempt = 0;
           attempt < 200 && (hasPending(outbox) || journalDeliveries < 2);
           ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      {
        std::scoped_lock lock(mutex);
        CHECK(journalDeliveries == 2);
        if (journalPayloads.size() == 2)
          CHECK(journalPayloads.front() == journalPayloads.back());
      }
    }

    // A publish is a JetStream publish, so the change subject needs a stream or
    // nothing is ever stored: a sink pointed at a stream that does not exist
    // yet owns creating it, and it must cover both subjects it publishes to --
    // a row that settles is the proof it did.
    {
      const std::string healed = stream + "-healed";
      const std::string healedChange = changeSubject + ".healed";
      const std::string healedAction = actionSubject + ".healed";
      NatsIdentityChangeSink healer(
          liveBus,
          NatsIdentityChangeSink::Config{.retryMs = 20,
                                         .changeSubject = healedChange,
                                         .actionSubject = healedAction,
                                         .streamName = healed});
      healer.reconcile();
      drogon::sync_wait(healer.publishCatalog(userCatalog(102, "Ana")));
      drogon::sync_wait(healer.publishAction(portraitRead(102)));
      for (int attempt = 0; attempt < 400 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      const auto info = streamStatus(liveBus->streamInfo(healed));
      CHECK(info.subjects.size() == 2);
    }

    // A backlog is one pass and not one tick per row: more changes than a single
    // batch holds all settle, and they are written before the drain starts so
    // that a drain fallen back to one row per pass cannot hide behind the wake
    // each enqueue gives it -- 100 rows at a 50 ms tick is five seconds.
    {
      const std::string burstStream = stream + "-burst";
      const std::string burstChange = changeSubject + ".burst";
      const std::string burstAction = actionSubject + ".burst";
      NatsIdentityChangeSink bursts(
          liveBus,
          NatsIdentityChangeSink::Config{.retryMs = 20,
                                         .changeSubject = burstChange,
                                         .actionSubject = burstAction,
                                         .streamName = burstStream});
      const auto started = std::chrono::steady_clock::now();
      for (int64_t recordId = 200; recordId < 300; ++recordId)
        drogon::sync_wait(
            bursts.publishCatalog(userCatalog(recordId, "Ana")));
      bursts.reconcile();
      for (int attempt = 0; attempt < 400 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      CHECK(std::chrono::steady_clock::now() - started <
            std::chrono::seconds(3));
    }

    // A publish that cannot succeed leaves its row pending with the attempt
    // counted, and the change queued behind it waits instead of overtaking it:
    // a wildcard is not a subject a broker stores a publish under, so nothing
    // is ever acknowledged.
    NatsIdentityChangeSink stranded(
        liveBus,
        NatsIdentityChangeSink::Config{.retryMs = 20,
                                       .changeSubject = changeSubject + ".*",
                                       .actionSubject = actionSubject,
                                       .streamName = stream});
    stranded.reconcile();
    drogon::sync_wait(stranded.publishCatalog(userCatalog(100, "Ana")));
    drogon::sync_wait(stranded.publishCatalog(userCatalog(101, "Ana")));

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
