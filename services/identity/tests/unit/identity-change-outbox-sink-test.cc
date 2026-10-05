#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/user/services/nats-identity-change-sink.hxx>
#include <outbox/outbox-key.hxx>
#include <outbox/outbox-repository.hxx>
#include <outbox/outbox-status.hxx>
#include <sqlite/db-service.hxx>
#include <nats/nats-bus.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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

outbox::OutboxRow pendingRow(const std::vector<outbox::OutboxRow>& rows)
{
  REQUIRE(!rows.empty());
  return rows.empty() ? outbox::OutboxRow{} : rows.front();
}

bool hasPending(const outbox::OutboxRepository& outbox)
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
      watermark, outbox::outboxStatusToString(outbox::OutboxStatus::Sent)));
}

bool isMintedActionId(const std::string& value)
{
  if (!value.starts_with(NatsIdentityChangeSink::kActionIdPrefix)
      || value.size() != NatsIdentityChangeSink::kActionIdPrefix.size() + 32)
    return false;
  const std::string_view hex =
      std::string_view(value).substr(NatsIdentityChangeSink::kActionIdPrefix.size());
  return std::ranges::all_of(hex, [](char character) {
    return (character >= '0' && character <= '9')
           || (character >= 'a' && character <= 'f');
  });
}

bool waitForDrain(const NatsIdentityChangeSink& sink,
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
}

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

  const auto outbox = NatsIdentityChangeSink::repository();

  {
    NatsIdentityChangeSink sink(nullptr, NatsIdentityChangeSink::Config{});

    drogon::sync_wait(sink.publishCatalog(userCatalog(42, "Ana")));
    const outbox::OutboxRow catalog = pendingRow(outbox.pendingBatch(1));
    CHECK(catalog.eventId.rfind("identity-change:", 0) == 0);
    CHECK(catalog.eventId.size() == 48);
    CHECK(catalog.subject == kChangeSubject);
    CHECK(catalog.payload.find("\"kind\":\"identity\"") != std::string::npos);
    CHECK(catalog.payload.find("\"table\":\"user\"") != std::string::npos);
    CHECK(catalog.payload.find("\"id\":42") != std::string::npos);
    CHECK(catalog.payload.find("\"deleted\":false") != std::string::npos);
    CHECK(catalog.payload.find("\"name\":\"Ana\"") != std::string::npos);
    CHECK(outbox.markSent(catalog.id, 1000));

    drogon::sync_wait(sink.publishCatalog(userCatalog(42, "Ana", true)));
    const outbox::OutboxRow deleted = pendingRow(outbox.pendingBatch(1));
    CHECK(deleted.payload.find("\"deleted\":true") != std::string::npos);

    CHECK(outbox.markSent(deleted.id, 1100));
    drogon::sync_wait(sink.publishCatalog(userCatalog(42, "Ana", true)));
    CHECK_FALSE(hasPending(outbox));

    drogon::sync_wait(sink.publishCatalog(userCatalog(42, "Ana Maria")));
    const outbox::OutboxRow moved = pendingRow(outbox.pendingBatch(1));
    CHECK(moved.eventId != catalog.eventId);
    CHECK(outbox.markSent(moved.id, 1200));

    drogon::sync_wait(sink.emitModule(
        {.table = TableName::User,
         .body = userRow(SyncOperation::Add, 42, "Ana"),
         .client = nullptr}));
    const outbox::OutboxRow emitted = pendingRow(outbox.pendingBatch(1));
    CHECK(emitted.eventId.rfind("identity-change:", 0) == 0);
    CHECK(emitted.payload.find("\"operation\":4") != std::string::npos);
    CHECK(emitted.payload.find("\"option\":\"user\"") != std::string::npos);
    CHECK(emitted.payload.find("\"info\":{") != std::string::npos);
    CHECK(emitted.payload.find("\"name\":\"Ana\"") != std::string::npos);
    CHECK(emitted.payload.find("\"kind\"") == std::string::npos);
    CHECK(emitted.payload.find("\"users\"") == std::string::npos);
    CHECK(outbox.markSent(emitted.id, 1300));

    drogon::sync_wait(sink.emitModule(
        {.table = TableName::UserInvitation,
         .body = userRow(SyncOperation::Add, 9, "Ana"),
         .client = nullptr}));
    const outbox::OutboxRow invitation = pendingRow(outbox.pendingBatch(1));
    CHECK(invitation.payload.find("\"option\":\"user_invitation\"") !=
          std::string::npos);
    CHECK(outbox.markSent(invitation.id, 1400));

    SocketEmitDto tombstone;
    tombstone.operation = SyncOperation::Delete;
    tombstone.option = TableName::User;
    tombstone.obj["id"] = static_cast<Json::Int64>(42);
    tombstone.obj["deletedAt"] = static_cast<Json::Int64>(1700000000);
    drogon::sync_wait(sink.emitModule(
        {.table = TableName::User, .body = tombstone, .client = nullptr}));
    const outbox::OutboxRow removed = pendingRow(outbox.pendingBatch(1));
    CHECK(removed.payload.find("\"operation\":5") != std::string::npos);
    CHECK(removed.payload.find("\"deletedAt\":1700000000") !=
          std::string::npos);
    CHECK(outbox.markSent(removed.id, 1500));

    SocketEmitDto anonymous;
    anonymous.operation = SyncOperation::Add;
    anonymous.option = TableName::User;
    anonymous.obj["name"] = "Ana";
    CHECK_THROWS_AS(
        drogon::sync_wait(sink.emitModule(
            {.table = TableName::User, .body = anonymous, .client = nullptr})),
        ResponseException);
    CHECK_FALSE(hasPending(outbox));

    SocketEmitDto misnamed = anonymous;
    misnamed.obj["id"] = "42";
    CHECK_THROWS_AS(
        drogon::sync_wait(sink.emitModule(
            {.table = TableName::User, .body = misnamed, .client = nullptr})),
        ResponseException);
    CHECK_FALSE(hasPending(outbox));

    drogon::sync_wait(sink.publishModuleAudit(invitationAudit(9)));
    const outbox::OutboxRow audited = pendingRow(outbox.pendingBatch(1));
    CHECK(audited.subject == kChangeSubject);
    CHECK(audited.payload.find("\"kind\":\"audit\"") != std::string::npos);
    CHECK(audited.payload.find("\"table_name\":\"user_invitation\"") !=
          std::string::npos);
    CHECK(audited.payload.find("\"record_id\":9") != std::string::npos);
    CHECK(audited.payload.find("\"create_user_id\":7") != std::string::npos);
    CHECK(audited.payload.find("\"current\":\"redeemed\"") != std::string::npos);
    CHECK(audited.payload.find("\"previous\":\"pending\"") != std::string::npos);
    CHECK(outbox.markSent(audited.id, 1600));

    ModuleAuditInput unchanged;
    unchanged.recordId = 10;
    unchanged.tableName = TableName::UserInvitation;
    unchanged.before["id"] = 10;
    unchanged.after = unchanged.before;
    drogon::sync_wait(sink.publishModuleAudit(unchanged));
    CHECK_FALSE(hasPending(outbox));

    drogon::sync_wait(sink.publishUsersAudit(userAudit(42, {42, 7})));
    const outbox::OutboxRow perUser = pendingRow(outbox.pendingBatch(1));
    CHECK(perUser.payload.find("\"kind\":\"audit\"") != std::string::npos);
    CHECK(perUser.payload.find("\"table_name\":\"user\"") !=
          std::string::npos);
    CHECK(perUser.payload.find("\"users\":[42,7]") != std::string::npos);
    CHECK(perUser.payload.find("\"current\":\"resident\"") !=
          std::string::npos);
    CHECK(outbox.markSent(perUser.id, 1700));

    drogon::sync_wait(sink.publishUsersAudit(userAudit(43, {42, 42, 0, -3})));
    const outbox::OutboxRow deduped = pendingRow(outbox.pendingBatch(1));
    CHECK(deduped.payload.find("\"users\":[42]") != std::string::npos);
    CHECK(deduped.payload.find("\"users\":[42,42]") == std::string::npos);
    CHECK(outbox.markSent(deduped.id, 1800));

    drogon::sync_wait(sink.publishUsersAudit(userAudit(44, {})));
    CHECK_FALSE(hasPending(outbox));

    drogon::sync_wait(
        sink.publishAction({.event = portraitRead(42), .client = nullptr}));
    const outbox::OutboxRow journal = pendingRow(outbox.pendingBatch(1));
    CHECK(journal.subject == kActionSubject);
    CHECK(journal.subject != kChangeSubject);
    CHECK(isMintedActionId(journal.eventId));
    CHECK(journal.payload.find("\"user_id\":7") != std::string::npos);
    CHECK(journal.payload.find("\"record_id\":42") != std::string::npos);
    CHECK(journal.payload.find("\"table_name\":\"user\"") != std::string::npos);
    CHECK(journal.payload.find("\"action\":\"read\"") != std::string::npos);
    CHECK(journal.payload.find("\"ip_address\":\"\"") != std::string::npos);
    CHECK(journal.payload.find("\"event\":\"portrait_preview\"") !=
          std::string::npos);
    CHECK(journal.payload.find("\"kind\"") == std::string::npos);
    CHECK(outbox.markSent(journal.id, 1900));

    drogon::sync_wait(
        sink.publishAction({.event = portraitRead(42), .client = nullptr}));
    const outbox::OutboxRow secondRead = pendingRow(outbox.pendingBatch(1));
    CHECK(secondRead.id != journal.id);
    CHECK(secondRead.payload == journal.payload);
    CHECK(isMintedActionId(secondRead.eventId));
    CHECK(secondRead.eventId != journal.eventId);
    CHECK(outbox.markSent(secondRead.id, 2000));

    IdentityCatalogInput oversized = userCatalog(8, "Ana");
    oversized.row["portrait"] =
        std::string(NatsIdentityChangeSink::kMaxPayloadBytes + 1, 'x');
    CHECK_THROWS_AS(drogon::sync_wait(sink.publishCatalog(oversized)),
                    ResponseException);
    CHECK_FALSE(hasPending(outbox));

    UserActionEvent oversizedAction = portraitRead(9);
    oversizedAction.newData["portrait"] =
        std::string(NatsIdentityChangeSink::kMaxPayloadBytes + 1, 'x');
    CHECK_THROWS_AS(drogon::sync_wait(sink.publishAction(
                        {.event = oversizedAction, .client = nullptr})),
                    ResponseException);
    CHECK_FALSE(hasPending(outbox));
  }

  {
    NatsIdentityChangeSink sink(nullptr, NatsIdentityChangeSink::Config{});
    sink.reconcile();
    drogon::sync_wait(sink.publishCatalog(userCatalog(12, "Ana")));
    drogon::sync_wait(
        sink.publishAction({.event = portraitRead(12), .client = nullptr}));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    const outbox::OutboxRow change = pendingRow(outbox.pendingBatch(1));
    CHECK(change.subject == kChangeSubject);
    CHECK(change.payload.find("\"id\":12") != std::string::npos);
    CHECK(change.attempts == 0);
    CHECK(outbox.markSent(change.id, 3000));

    const outbox::OutboxRow action = pendingRow(outbox.pendingBatch(1));
    CHECK(action.subject == kActionSubject);
    CHECK(isMintedActionId(action.eventId));
    CHECK(action.attempts == 0);
    CHECK_FALSE(sink.drained());
    sink.requestStop();
    CHECK(waitForDrain(sink, std::chrono::seconds(5)));
    sink.requestStop();
    CHECK(sink.drained());
    CHECK(outbox.markSent(action.id, 3100));
    CHECK_FALSE(hasPending(outbox));
  }

  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url != nullptr && *url != '\0') {
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

    std::mutex mutex;
    std::condition_variable cv;
    std::string changed;
    std::string changedMsgId;
    const auto changeSubscription = liveBus->subscribeDurable(
        {.stream = stream,
         .durable = "identity-change-outbox-live-" + run,
         .subject = changeSubject,
         .deliverAll = true,
         .maxDeliver = 3,
         .maxAckPending = NatsBus::kDefaultMaxAckPending,
         .handler = [&mutex, &cv, &changed, &changedMsgId](
                        const NatsBus::DurableMessage& message,
                        const NatsBus::DurableSettlement& settlement) {
           {
             std::scoped_lock lock(mutex);
             changed = std::string(message.payload);
             changedMsgId = std::string(message.msgId);
           }
           settlement.ack();
           cv.notify_all();
         }});
    REQUIRE(changeSubscription.has_value());

    std::size_t journalDeliveries = 0;
    std::vector<std::string> journalPayloads;
    std::vector<std::string> journalMsgIds;
    const auto actionSubscription = liveBus->subscribeDurable(
        {.stream = stream,
         .durable = "identity-action-journal-live-" + run,
         .subject = actionSubject,
         .deliverAll = true,
         .maxDeliver = 3,
         .maxAckPending = NatsBus::kDefaultMaxAckPending,
         .handler = [&mutex, &cv, &journalDeliveries, &journalPayloads,
                     &journalMsgIds](
                        const NatsBus::DurableMessage& message,
                        const NatsBus::DurableSettlement& settlement) {
           {
             std::scoped_lock lock(mutex);
             ++journalDeliveries;
             journalPayloads.emplace_back(message.payload);
             journalMsgIds.emplace_back(message.msgId);
           }
           settlement.ack();
           cv.notify_all();
         }});
    REQUIRE(actionSubscription.has_value());

    {
      NatsIdentityChangeSink liveSink(
          liveBus,
          NatsIdentityChangeSink::Config{.retryMs = 20,
                                         .changeSubject = changeSubject,
                                         .actionSubject = actionSubject,
                                         .streamName = stream});
      drogon::sync_wait(liveSink.publishCatalog(userCatalog(99, "Ana")));
      const auto catalogRow = pendingRow(outbox.pendingBatch(1));
      const std::string expected = catalogRow.payload;
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
      std::string seenMsgId;
      {
        std::scoped_lock lock(mutex);
        seen = changed;
        seenMsgId = changedMsgId;
      }
      CHECK(seen == expected);
      CHECK(seenMsgId == catalogRow.eventId);

      for (int i = 0; i < 2; ++i)
        drogon::sync_wait(
            liveSink.publishAction({.event = portraitRead(99),
                                    .client = nullptr}));
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
        if (journalMsgIds.size() == 2) {
          CHECK(isMintedActionId(journalMsgIds.front()));
          CHECK(isMintedActionId(journalMsgIds.back()));
          CHECK(journalMsgIds.front() != journalMsgIds.back());
        }
      }
    }

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
      drogon::sync_wait(healer.publishAction(
          {.event = portraitRead(102), .client = nullptr}));
      for (int attempt = 0; attempt < 400 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      const auto info = streamStatus(liveBus->streamInfo(healed));
      CHECK(info.subjects.size() == 2);
    }

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
      const int64_t watermark = outboxWatermark();
      const auto started = std::chrono::steady_clock::now();
      for (int64_t recordId = 200; recordId < 300; ++recordId)
        drogon::sync_wait(
            bursts.publishCatalog(userCatalog(recordId, "Ana")));
      CHECK(rowsAfter(watermark) == 100);
      bursts.reconcile();
      for (int attempt = 0; attempt < 400 && hasPending(outbox); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      CHECK_FALSE(hasPending(outbox));
      CHECK(sentRowsAfter(watermark) == 100);
      CHECK(std::chrono::steady_clock::now() - started <
            std::chrono::seconds(3));
    }

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
