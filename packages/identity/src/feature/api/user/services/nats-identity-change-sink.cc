#include "nats-identity-change-sink.hxx"

#include <chrono>
#include <exception>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/sync-change.hxx>
#include <sync/user-audit-event.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>
#include <utility>

#include <unordered_set>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kStuckLogEvery = 100;
constexpr int kEnqueueAttempts = 3;
constexpr int kEnqueueRetryMs = 25;
constexpr int kDrainBatch = 64;
constexpr int kProgressMs = 50;
} // namespace

NatsIdentityChangeSink::NatsIdentityChangeSink(std::shared_ptr<NatsBus> bus,
                                               Config config)
    : bus_(std::move(bus)),
      config_(std::move(config)),
      changeSubject_(config_.changeSubject.empty()
                         ? std::string(nats_subject::kIdentityChange)
                         : config_.changeSubject),
      actionSubject_(config_.actionSubject.empty()
                         ? std::string(nats_subject::kIdentityUserAction)
                         : config_.actionSubject),
      stream_(config_.streamName.empty() ? std::string("ARGUS_IDENTITY_CHANGE")
                                        : config_.streamName)
{
}

NatsIdentityChangeSink::~NatsIdentityChangeSink()
{
  stopping_.store(true, std::memory_order_release);
  wake_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

drogon::Task<void>
NatsIdentityChangeSink::publishCatalog(const IdentityCatalogInput& input) const
{
  Json::Value event(Json::objectValue);
  event[sync_change::kKindField] = sync_change::kKindIdentity;
  event["table"] = tableNameToString(input.table);
  event["id"] = static_cast<Json::Int64>(input.id);
  event["deleted"] = input.deleted;
  event["row"] = input.row;
  const std::string payload = json_util::toString(event);
  const std::string id =
      change_outbox_key::eventId({.table = tableNameToString(input.table),
                                  .recordId = input.id,
                                  .discriminator = payload});
  co_await enqueueChange(id, payload);
}

drogon::Task<void>
NatsIdentityChangeSink::emitModule(TableName table,
                                   const SocketEmitDto& body) const
{
  const Json::Value& recordId = body.obj["id"];
  if (!recordId.isIntegral()) {
    LOG_ERROR << "Identity change outbox: an emit of "
              << tableNameToString(table)
              << " carried no record id; not recorded";
    co_return;
  }
  SocketEmitDto frame = body;
  frame.option = table;
  const std::string payload =
      json_util::toString(sync_change::emitPayload(frame));
  const std::string id =
      change_outbox_key::eventId({.table = tableNameToString(table),
                                  .recordId = recordId.asInt64(),
                                  .discriminator = payload});
  co_await enqueueChange(id, payload);
}

drogon::Task<void>
NatsIdentityChangeSink::publishModuleAudit(const ModuleAuditInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  ModuleAuditEvent event;
  event.recordId = input.recordId;
  event.tableName = input.tableName;
  event.changes = changes;
  event.createUserId = input.actorId;
  event.eventTimestamp = nowMs();
  const std::string payload = json_util::toString(event.toJson());
  const std::string id =
      change_outbox_key::eventId({.table = tableNameToString(input.tableName),
                                  .recordId = input.recordId,
                                  .discriminator = payload});
  co_await enqueueChange(id, payload);
}

drogon::Task<void>
NatsIdentityChangeSink::publishUsersAudit(const UserAuditInput& input) const
{
  const auto changes = JsonDiff::createFlatDiff(input.before, input.after);
  if (changes.empty())
    co_return;

  std::vector<int64_t> recipients;
  std::unordered_set<int64_t> seen;
  for (const auto userId : input.userIds) {
    if (userId <= 0 || !seen.insert(userId).second)
      continue;
    recipients.push_back(userId);
  }
  if (recipients.empty())
    co_return;

  UserAuditEvent event;
  event.recordId = input.recordId;
  event.tableName = input.tableName;
  event.changes = changes;
  event.users = std::move(recipients);
  event.eventTimestamp = nowMs();
  const std::string payload = json_util::toString(event.toJson());
  const std::string id =
      change_outbox_key::eventId({.table = tableNameToString(input.tableName),
                                  .recordId = input.recordId,
                                  .discriminator = payload});
  co_await enqueueChange(id, payload);
}

drogon::Task<void>
NatsIdentityChangeSink::publishAction(const UserActionEvent& event) const
{
  co_await enqueueAction(json_util::toString(event.toJson()));
}

drogon::Task<void>
NatsIdentityChangeSink::enqueueChange(std::string eventId,
                                      std::string payloadJson) const
{
  if (payloadJson.size() > kMaxPayloadBytes) {
    LOG_ERROR << "Identity change outbox: " << eventId << " carries "
              << payloadJson.size()
              << " bytes, past the broker's message budget; not recorded";
    co_return;
  }
  const ChangeOutboxEnqueueInput input{
      .eventId = std::move(eventId),
      .subject = changeSubject_,
      .fingerprint = change_outbox_key::fingerprintJson(payloadJson),
      .payload = std::move(payloadJson),
      .at = nowMs(),
  };
  for (int attempt = 0; attempt < kEnqueueAttempts; ++attempt) {
    if (co_await outbox_.enqueue(input) != ChangeOutboxDisposition::Failed) {
      wake_.notify_all();
      co_return;
    }
    co_await drogon::sleepCoro(
        trantor::EventLoop::getEventLoopOfCurrentThread(),
        std::chrono::milliseconds(kEnqueueRetryMs));
  }
  LOG_ERROR << "Identity change outbox: " << input.eventId
            << " could not be recorded in " << kEnqueueAttempts
            << " attempts; the change is lost";
}

drogon::Task<void>
NatsIdentityChangeSink::enqueueAction(std::string payloadJson) const
{
  if (payloadJson.size() > kMaxPayloadBytes) {
    LOG_ERROR << "Identity change outbox: an action journal row carries "
              << payloadJson.size()
              << " bytes, past the broker's message budget; not recorded";
    co_return;
  }
  const ChangeOutboxActionInput input{
      .subject = actionSubject_,
      .fingerprint = change_outbox_key::fingerprintJson(payloadJson),
      .payload = std::move(payloadJson),
      .at = nowMs(),
  };
  for (int attempt = 0; attempt < kEnqueueAttempts; ++attempt) {
    if (co_await outbox_.enqueueAction(input)) {
      wake_.notify_all();
      co_return;
    }
    co_await drogon::sleepCoro(
        trantor::EventLoop::getEventLoopOfCurrentThread(),
        std::chrono::milliseconds(kEnqueueRetryMs));
  }
  LOG_ERROR << "Identity change outbox: an action journal row could not be "
               "recorded in "
            << kEnqueueAttempts << " attempts; the action is lost";
}

void NatsIdentityChangeSink::reconcile()
{
  if (!workerStarted_.exchange(true, std::memory_order_acq_rel))
    worker_ = std::thread([this]() { flushLoop(); });
}

bool NatsIdentityChangeSink::ensureStream() const
{
  constexpr int64_t kJetStreamRetentionNs = 7LL * 24 * 60 * 60 * 1000000000;
  constexpr int64_t kJetStreamDuplicatesNs = 2LL * 60 * 1000000000;
  if (!bus_->ensureStream({.name = stream_,
                           .subjects = {changeSubject_, actionSubject_},
                           .maxAgeNs = kJetStreamRetentionNs,
                           .duplicatesNs = kJetStreamDuplicatesNs}))
    return false;
  LOG_INFO << "Identity change outbox: stream " << stream_
           << " ready (7d retention)";
  return true;
}

bool NatsIdentityChangeSink::flush(const ChangeOutboxRow& row)
{
  if (!bus_ || !bus_->isConnected())
    return false;
  if (!streamReady_.load(std::memory_order_acquire))
    streamReady_.store(ensureStream(), std::memory_order_release);

  // The journal leg names the row's own position, because the transition it
  // records has no content of its own to be keyed by: two reads of one record
  // are two audit rows.
  const std::string msgId = row.eventId.empty()
                                ? change_outbox_key::actionMsgId(row.id)
                                : row.eventId;
  if (bus_->publishWithMsgId(
          {.subject = row.subject, .payload = row.payload, .msgId = msgId})) {
    if (!outbox_.markSent(row.id, nowMs())) {
      LOG_WARN << "Identity change outbox: " << msgId
               << " was stored but could not be marked sent; it stays pending";
      return false;
    }
    return true;
  }
  streamReady_.store(false, std::memory_order_relaxed);
  static_cast<void>(outbox_.recordAttempt(row.id));
  const int64_t attempts = row.attempts + 1;
  if (attempts <= 1 || attempts % kStuckLogEvery == 0)
    LOG_WARN << "Identity change outbox: " << msgId
             << " is still unpublished after " << attempts
             << " attempts; every later change waits behind it";
  return false;
}

void NatsIdentityChangeSink::flushLoop()
{
  while (!stopping_.load(std::memory_order_acquire)) {
    bool progressed = false;
    try {
      for (const auto& row : outbox_.pendingBatch(kDrainBatch)) {
        if (stopping_.load(std::memory_order_acquire) || !flush(row))
          break;
        progressed = true;
      }
    }
    catch (const std::exception& e) {
      LOG_WARN << "Identity change outbox: flush failed (" << e.what()
               << "); retrying";
    }
    std::unique_lock lock(wakeMutex_);
    wake_.wait_for(lock,
                   std::chrono::milliseconds(progressed ? kProgressMs
                                                        : config_.retryMs));
  }
}
