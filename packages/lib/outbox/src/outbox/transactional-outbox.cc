#include "transactional-outbox.hxx"

#include "outbox-key.hxx"

#include <chrono>
#include <errors/response-exception.hxx>
#include <exception>
#include <nats/nats-bus.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

namespace outbox
{

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kStuckLogEvery = 100;
}

TransactionalOutbox::TransactionalOutbox(std::shared_ptr<NatsBus> bus,
                                         OutboxConfig config)
    : bus_(std::move(bus)),
      config_(std::move(config)),
      repository_(config_.client)
{
}

TransactionalOutbox::~TransactionalOutbox()
{
  requestStop();
  if (worker_.joinable())
    worker_.join();
}

void TransactionalOutbox::refuseOversized(std::string_view what,
                                          std::size_t bytes) const
{
  if (bytes <= kMaxPayloadBytes)
    return;
  LOG_ERROR << config_.label << ": " << std::string(what) << " carries " << bytes
            << " bytes, past the broker's message budget; the write is "
               "refused";
  throw ResponseException(config_.refusal);
}

drogon::Task<OutboxDisposition>
TransactionalOutbox::record(OutboxRecordInput input) const
{
  refuseOversized(input.eventId, input.payload.size());
  const OutboxInsert row{.eventId = std::move(input.eventId),
                         .subject = std::move(input.subject),
                         .fingerprint = fingerprint(input.payload),
                         .payload = std::move(input.payload),
                         .at = nowMs(),
                         .client = input.client};
  const auto disposition = co_await repository_.insert(row);
  if (disposition == OutboxDisposition::Enqueued)
    wake_.notify();
  else if (disposition == OutboxDisposition::Conflict)
    LOG_ERROR << config_.label << ": " << row.eventId
              << " already holds a different transition; not dispatching it";
  co_return disposition;
}

drogon::Task<void> TransactionalOutbox::append(OutboxAppendInput input) const
{
  refuseOversized(input.subject, input.payload.size());
  const auto entropy = drawEntropy();
  if (!entropy)
    throw ResponseException(503, config_.refusal);
  const OutboxInsert row{.eventId = uniqueId(input.idPrefix, *entropy),
                         .subject = std::move(input.subject),
                         .fingerprint = fingerprint(input.payload),
                         .payload = std::move(input.payload),
                         .at = nowMs(),
                         .client = input.client};
  if (co_await repository_.insert(row) != OutboxDisposition::Enqueued) {
    LOG_ERROR << config_.label << ": the minted msg id " << row.eventId
              << " is already taken; the write is refused";
    throw ResponseException(config_.refusal);
  }
  wake_.notify();
}

bool TransactionalOutbox::migrateSchema() const
{
  return repository_.migrateSchema();
}

void TransactionalOutbox::reconcile()
{
  if (!workerStarted_.exchange(true, std::memory_order_acq_rel))
    worker_ = std::thread([this]() { flushLoop(); });
}

void TransactionalOutbox::requestStop()
{
  stopping_.store(true, std::memory_order_release);
  wake_.notify();
}

bool TransactionalOutbox::drained() const
{
  return exited_.load(std::memory_order_acquire) ||
         !workerStarted_.load(std::memory_order_acquire);
}

bool TransactionalOutbox::ensureStreams()
{
  if (!bus_ || !bus_->isConnected())
    return false;
  if (streamReady_.load(std::memory_order_acquire))
    return true;
  const bool ready = !config_.ensureStreams || config_.ensureStreams(*bus_);
  streamReady_.store(ready, std::memory_order_release);
  return ready;
}

bool TransactionalOutbox::flush(const OutboxRow& row)
{
  if (!bus_ || !bus_->isConnected())
    return false;
  static_cast<void>(ensureStreams());

  const std::string& subject =
      row.subject.empty() ? config_.defaultSubject : row.subject;
  const std::string msgId = row.eventId.empty()
                                ? legacyId(config_.legacyIdPrefix, row.id)
                                : row.eventId;
  if (bus_->publishWithMsgId(
          {.subject = subject, .payload = row.payload, .msgId = msgId})) {
    if (!repository_.markSent(row.id, nowMs())) {
      LOG_WARN << config_.label << ": " << msgId
               << " was stored but could not be marked sent; it stays pending";
      return false;
    }
    return true;
  }
  streamReady_.store(false, std::memory_order_relaxed);
  static_cast<void>(repository_.recordAttempt(row.id));
  const int64_t attempts = row.attempts + 1;
  if (attempts <= 1 || attempts % kStuckLogEvery == 0)
    LOG_WARN << config_.label << ": " << msgId << " is still unpublished after "
             << attempts << " attempts; every later change waits behind it";
  return false;
}

void TransactionalOutbox::purgeSettled()
{
  const int64_t now = nowMs();
  if (now < nextPurgeMs_)
    return;
  nextPurgeMs_ = now + config_.retention.purgeEveryMs;
  try {
    const int64_t purged =
        repository_.purgeSent(now - config_.retention.keepSentMs);
    if (purged > 0)
      LOG_INFO << config_.label << ": purged " << purged
               << " settled row(s) past the stream's retention";
  }
  catch (const std::exception& e) {
    nextPurgeMs_ = now + config_.retention.purgeRetryMs;
    LOG_WARN << config_.label << ": purge failed (" << e.what()
             << "); the settled rows stay and the purge is retried";
  }
}

void TransactionalOutbox::flushLoop()
{
  int failures = 0;
  while (!stopping_.load(std::memory_order_acquire)) {
    bool progressed = false;
    bool blocked = false;
    try {
      static_cast<void>(ensureStreams());
      for (const auto& row : repository_.pendingBatch(config_.timing.batch)) {
        if (stopping_.load(std::memory_order_acquire))
          break;
        if (!flush(row)) {
          blocked = true;
          break;
        }
        progressed = true;
      }
    }
    catch (const std::exception& e) {
      blocked = true;
      LOG_WARN << config_.label << ": flush failed (" << e.what()
               << "); retrying";
    }
    purgeSettled();
    failures = blocked && !progressed ? failures + 1 : 0;
    wake_.waitFor(progressed
                      ? std::chrono::milliseconds(config_.timing.progressMs)
                      : retryDelay(config_.timing, failures));
  }
  exited_.store(true, std::memory_order_release);
}

}
