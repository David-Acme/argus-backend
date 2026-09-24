#include "auth-action-sink.hxx"

#include <array>
#include <chrono>
#include <errors/response-exception.hxx>
#include <exception>
#include <openssl/rand.h>
#include <feature/session/repositories/change-outbox/change-outbox-key.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <sync/stream-retention.hxx>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kStuckLogEvery = 100;

std::string mintedActionMsgId()
{
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw ResponseException(503, AuthErrors::ChangeNotRecorded);
  return change_outbox_key::actionMsgId(bytes);
}

constexpr int kDrainBatch = 64;
constexpr int kProgressMs = 50;
}

AuthActionSink::AuthActionSink(std::shared_ptr<NatsBus> bus, Config config)
    : bus_(std::move(bus)),
      config_(std::move(config)),
      actionSubject_(config_.actionSubject.empty()
                         ? std::string(nats_subject::kAuthUserAction)
                         : config_.actionSubject),
      stream_(config_.streamName.empty()
                  ? std::string(nats_subject::kAuthChangeStream)
                  : config_.streamName)
{
}

AuthActionSink::~AuthActionSink()
{
  requestStop();
  if (worker_.joinable())
    worker_.join();
}

drogon::Task<void>
AuthActionSink::publishAction(const AuthActionPublishInput& input) const
{
  co_await enqueueAction(json_util::toString(input.event.toJson()),
                         input.client);
}

drogon::Task<void>
AuthActionSink::enqueueAction(std::string payloadJson,
                              drogon::orm::DbClient* client) const
{
  if (payloadJson.size() > kMaxPayloadBytes) {
    LOG_ERROR << "Auth action outbox: an action journal row carries "
              << payloadJson.size()
              << " bytes, past the broker's message budget; the write is "
                 "refused";
    throw ResponseException(AuthErrors::ChangeNotRecorded);
  }
  const ChangeOutboxActionInput input{
      .eventId = mintedActionMsgId(),
      .subject = actionSubject_,
      .fingerprint = change_outbox_key::fingerprintJson(payloadJson),
      .payload = std::move(payloadJson),
      .at = nowMs(),
      .client = client,
  };
  co_await outbox_.enqueueAction(input);
  wake_.notify_all();
}

void AuthActionSink::reconcile()
{
  if (!workerStarted_.exchange(true, std::memory_order_acq_rel))
    worker_ = std::thread([this]() { flushLoop(); });
}

void AuthActionSink::requestStop()
{
  stopping_.store(true, std::memory_order_release);
  wake_.notify_all();
}

bool AuthActionSink::drained() const
{
  return exited_.load(std::memory_order_acquire) ||
         !workerStarted_.load(std::memory_order_acquire);
}

bool AuthActionSink::ensureStream() const
{
  if (!bus_->ensureStream({.name = stream_,
                           .subjects = {actionSubject_},
                           .maxAgeNs = stream_retention::kRetentionNs,
                           .duplicatesNs = stream_retention::kDuplicatesNs}))
    return false;
  LOG_INFO << "Auth action outbox: stream " << stream_ << " ready";
  return true;
}

bool AuthActionSink::flush(const ChangeOutboxRow& row)
{
  if (!bus_ || !bus_->isConnected())
    return false;
  if (!streamReady_.load(std::memory_order_acquire))
    streamReady_.store(ensureStream(), std::memory_order_release);

  const std::string msgId = row.eventId.empty()
                                ? change_outbox_key::legacyActionMsgId(row.id)
                                : row.eventId;
  if (bus_->publishWithMsgId(
          {.subject = row.subject, .payload = row.payload, .msgId = msgId})) {
    if (!outbox_.markSent(row.id, nowMs())) {
      LOG_WARN << "Auth action outbox: " << msgId
               << " was stored but could not be marked sent; it stays pending";
      return false;
    }
    return true;
  }
  streamReady_.store(false, std::memory_order_relaxed);
  static_cast<void>(outbox_.recordAttempt(row.id));
  const int64_t attempts = row.attempts + 1;
  if (attempts <= 1 || attempts % kStuckLogEvery == 0)
    LOG_WARN << "Auth action outbox: " << msgId
             << " is still unpublished after " << attempts
             << " attempts; every later change waits behind it";
  return false;
}

void AuthActionSink::flushLoop()
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
      LOG_WARN << "Auth action outbox: flush failed (" << e.what()
               << "); retrying";
    }
    const int64_t now = nowMs();
    if (now >= nextPurgeMs_) {
      nextPurgeMs_ = now + stream_retention::kSettledPurgeIntervalMs;
      try {
        const int64_t purged =
            outbox_.purgeSent(now - stream_retention::kRetentionMs);
        if (purged > 0)
          LOG_INFO << "Auth action outbox: purged " << purged
                   << " settled row(s) past the stream's retention";
      }
      catch (const std::exception& e) {
        nextPurgeMs_ = now + stream_retention::kSettledPurgeRetryMs;
        LOG_WARN << "Auth action outbox: purge failed (" << e.what()
                 << "); the settled rows stay and the purge is retried";
      }
    }
    std::unique_lock lock(wakeMutex_);
    wake_.wait_for(lock,
                   std::chrono::milliseconds(progressed ? kProgressMs
                                                        : config_.retryMs));
  }
  exited_.store(true, std::memory_order_release);
}
