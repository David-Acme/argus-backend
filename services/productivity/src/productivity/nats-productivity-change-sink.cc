#include "nats-productivity-change-sink.hxx"

#include <chrono>
#include <exception>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
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
}

NatsProductivityChangeSink::NatsProductivityChangeSink(
    std::shared_ptr<NatsBus> bus, Config config)
    : bus_(std::move(bus)),
      config_(std::move(config)),
      subject_(config_.publishSubject.empty()
                   ? std::string(nats_subject::kProductivityChange)
                   : config_.publishSubject),
      stream_(config_.streamName.empty() ? std::string("ARGUS_PRODUCTIVITY_CHANGE")
                                        : config_.streamName)
{
}

NatsProductivityChangeSink::~NatsProductivityChangeSink()
{
  stopping_.store(true, std::memory_order_release);
  wake_.notify_all();
  if (worker_.joinable())
    worker_.join();
}

drogon::Task<void>
NatsProductivityChangeSink::emitUser(int64_t userId,
                                     const SocketEmitDto& body) const
{
  co_await emit({userId}, body);
}

drogon::Task<void>
NatsProductivityChangeSink::emitUsers(const std::vector<int64_t>& userIds,
                                      const SocketEmitDto& body) const
{
  co_await emit(userIds, body);
}

drogon::Task<void>
NatsProductivityChangeSink::emit(const std::vector<int64_t>& userIds,
                                 const SocketEmitDto& body) const
{
  const Json::Value& recordId = body.obj["id"];
  if (!recordId.isIntegral()) {
    LOG_ERROR << "Productivity change outbox: an emit of "
              << tableNameToString(body.option)
              << " carried no record id; not recorded";
    co_return;
  }
  const std::string payload =
      json_util::toString(sync_change::userEmitPayload(body, userIds));
  const std::string id =
      change_outbox_key::eventId({.table = tableNameToString(body.option),
                                  .recordId = recordId.asInt64(),
                                  .discriminator = payload});
  co_await enqueue(id, payload);
}

drogon::Task<void>
NatsProductivityChangeSink::publishAudit(const UserAuditInput& input) const
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
  co_await enqueue(id, payload);
}

drogon::Task<void>
NatsProductivityChangeSink::enqueue(std::string eventId,
                                    std::string payloadJson) const
{
  if (payloadJson.size() > kMaxPayloadBytes) {
    LOG_ERROR << "Productivity change outbox: " << eventId << " carries "
              << payloadJson.size()
              << " bytes, past the broker's message budget; not recorded";
    co_return;
  }
  const std::string fingerprint =
      change_outbox_key::fingerprintJson(payloadJson);
  const ChangeOutboxEnqueueInput input{
      .eventId = std::move(eventId),
      .fingerprint = fingerprint,
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
  LOG_ERROR << "Productivity change outbox: " << input.eventId
            << " could not be recorded in " << kEnqueueAttempts
            << " attempts; the change is lost";
}

void NatsProductivityChangeSink::reconcile()
{
  if (!workerStarted_.exchange(true, std::memory_order_acq_rel))
    worker_ = std::thread([this]() { flushLoop(); });
}

bool NatsProductivityChangeSink::ensureStream() const
{
  constexpr int64_t kJetStreamRetentionNs = 7LL * 24 * 60 * 60 * 1000000000;
  constexpr int64_t kJetStreamDuplicatesNs = 2LL * 60 * 1000000000;
  if (!bus_->ensureStream({.name = stream_,
                           .subjects = {subject_},
                           .maxAgeNs = kJetStreamRetentionNs,
                           .duplicatesNs = kJetStreamDuplicatesNs}))
    return false;
  LOG_INFO << "Productivity change outbox: stream " << stream_
           << " ready (7d retention)";
  return true;
}

bool NatsProductivityChangeSink::flush(const ChangeOutboxRow& row)
{
  if (!bus_ || !bus_->isConnected())
    return false;
  if (!streamReady_.load(std::memory_order_acquire))
    streamReady_.store(ensureStream(), std::memory_order_release);

  if (bus_->publishWithMsgId(
          {.subject = subject_, .payload = row.payload, .msgId = row.eventId})) {
    if (!outbox_.markSent(row.eventId, nowMs())) {
      LOG_WARN << "Productivity change outbox: " << row.eventId
               << " was stored but could not be marked sent; it stays pending";
      return false;
    }
    return true;
  }
  streamReady_.store(false, std::memory_order_relaxed);
  static_cast<void>(outbox_.recordAttempt(row.eventId));
  const int64_t attempts = row.attempts + 1;
  if (attempts <= 1 || attempts % kStuckLogEvery == 0)
    LOG_WARN << "Productivity change outbox: " << row.eventId
             << " is still unpublished after " << attempts
             << " attempts; every later change waits behind it";
  return false;
}

void NatsProductivityChangeSink::flushLoop()
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
      LOG_WARN << "Productivity change outbox: flush failed (" << e.what()
               << "); retrying";
    }
    std::unique_lock lock(wakeMutex_);
    wake_.wait_for(lock,
                   std::chrono::milliseconds(progressed ? kProgressMs
                                                        : config_.retryMs));
  }
}
