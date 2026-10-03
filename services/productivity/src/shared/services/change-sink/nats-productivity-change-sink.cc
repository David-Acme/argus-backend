#include "nats-productivity-change-sink.hxx"

#include <chrono>
#include <errors/response-exception.hxx>
#include <exception>
#include <productivity/productivity-errors.hxx>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
#include <sync/stream-retention.hxx>
#include <sync/sync-change.hxx>
#include <sync/user-audit-event.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
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
      stream_(config_.streamName.empty()
                  ? std::string(nats_subject::kProductivityChangeStream)
                  : config_.streamName)
{
}

NatsProductivityChangeSink::~NatsProductivityChangeSink()
{
  requestStop();
  if (worker_.joinable())
    worker_.join();
}

drogon::Task<void>
NatsProductivityChangeSink::emitUsers(const UserEmitInput& input) const
{
  const Json::Value& recordId = input.body.obj["id"];
  if (!recordId.isIntegral()) {
    LOG_ERROR << "Productivity change outbox: an emit of "
              << tableNameToString(input.body.option)
              << " carried no record id; the write is refused";
    throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  const std::string payload = json_util::toString(
      sync_change::userEmitPayload(input.body, input.userIds));
  const std::string transition =
      payload + '|' + std::to_string(nowMs()) + '|' +
      std::to_string(transitions_.fetch_add(1, std::memory_order_relaxed));
  const std::string id =
      change_outbox_key::eventId({.table = tableNameToString(input.body.option),
                                  .recordId = recordId.asInt64(),
                                  .discriminator = transition});
  co_await enqueue(
      {.eventId = id, .payload = payload, .client = input.client});
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
  co_await enqueue(
      {.eventId = id, .payload = payload, .client = input.client});
}

drogon::Task<void>
NatsProductivityChangeSink::enqueue(ChangeOutboxEnqueueInput input) const
{
  if (input.payload.size() > kMaxPayloadBytes) {
    LOG_ERROR << "Productivity change outbox: " << input.eventId << " carries "
              << input.payload.size()
              << " bytes, past the broker's message budget; the write is "
                 "refused";
    throw ResponseException(ProductivityErrors::ChangeNotRecorded);
  }
  input.fingerprint = change_outbox_key::fingerprintJson(input.payload);
  input.at = nowMs();
  co_await outbox_.enqueue(input);
  wake_.notify();
}

void NatsProductivityChangeSink::reconcile()
{
  if (!workerStarted_.exchange(true, std::memory_order_acq_rel))
    worker_ = std::thread([this]() { flushLoop(); });
}

void NatsProductivityChangeSink::requestStop()
{
  stopping_.store(true, std::memory_order_release);
  wake_.notify();
}

bool NatsProductivityChangeSink::drained() const
{
  return exited_.load(std::memory_order_acquire) ||
         !workerStarted_.load(std::memory_order_acquire);
}

bool NatsProductivityChangeSink::ensureStream() const
{
  if (!bus_->ensureStream({.name = stream_,
                           .subjects = {subject_},
                           .maxAgeNs = stream_retention::kRetentionNs,
                           .duplicatesNs = stream_retention::kDuplicatesNs}))
    return false;
  LOG_INFO << "Productivity change outbox: stream " << stream_ << " ready";
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
      if (!streamReady_.load(std::memory_order_acquire) && bus_ &&
          bus_->isConnected())
        streamReady_.store(ensureStream(), std::memory_order_release);
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
    const int64_t now = nowMs();
    if (now >= nextPurgeMs_) {
      nextPurgeMs_ = now + stream_retention::kSettledPurgeIntervalMs;
      try {
        const int64_t purged =
            outbox_.purgeSent(now - stream_retention::kRetentionMs);
        if (purged > 0)
          LOG_INFO << "Productivity change outbox: purged " << purged
                   << " settled row(s) past the stream's retention";
      }
      catch (const std::exception& e) {
        nextPurgeMs_ = now + stream_retention::kSettledPurgeRetryMs;
        LOG_WARN << "Productivity change outbox: purge failed (" << e.what()
                 << "); the settled rows stay and the purge is retried";
      }
    }
    wake_.waitFor(std::chrono::milliseconds(progressed ? kProgressMs
                                                     : config_.retryMs));
  }
  exited_.store(true, std::memory_order_release);
}
