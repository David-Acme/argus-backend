#include <operator/nats-object-event-sink.hxx>

#include <shared/services/event-stream/event-stream.hxx>
#include <sync/stream-retention.hxx>
#include <text/json-util.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

namespace
{
std::string makeSessionTag()
{
  std::random_device device;
  std::uniform_int_distribution<uint64_t> distribution;
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << distribution(device)
      << std::setw(16) << distribution(device);
  return out.str();
}
}

NatsObjectEventSink::NatsObjectEventSink(std::shared_ptr<NatsBus> bus,
                                         Config config)
    : bus_(std::move(bus)),
      config_(config),
      sessionTag_(config_.sessionTag.empty() ? makeSessionTag()
                                             : config_.sessionTag)
{
}

NatsObjectEventSink::~NatsObjectEventSink()
{
  requestStop();
  if (worker_.joinable())
    worker_.join();
}

ObjectEventPublishResult
NatsObjectEventSink::publish(const ObjectDetectedEvent& event)
{
  if (!bus_)
    return ObjectEventPublishResult::Failed;
  ObjectDetectedEvent stored = event;
  if (stored.eventId.empty())
    stored.eventId = std::to_string(stored.cameraId) + ":" +
                     std::to_string(nowMs()) + ":" +
                     std::to_string(nextSequence_.fetch_add(1));

  std::vector<std::string> cooldownClasses;
  if (stored.rule.rfind("person", 0) == 0 && stored.trackId > 0) {
    cooldownClasses.push_back("person:" + sessionTag_ + ":" +
                              std::to_string(stored.trackId));
  }
  else {
    for (const auto& object : stored.objects)
      cooldownClasses.push_back(object.name);
  }

  const ObjectEventEnqueueOutcome outcome = outbox_.enqueue(
      {.eventId = stored.eventId,
       .payload = json_util::toString(object_event::toJson(stored)),
       .cameraId = stored.cameraId,
       .cooldownClasses = std::move(cooldownClasses),
       .nowMs = nowMs(),
       .cooldownMs = config_.cooldownMs,
       .maxPending = config_.maxPending});
  if (outcome.result == ObjectEventEnqueueResult::Suppressed) {
    LOG_INFO << "Camera operator: event suppressed by the persisted cooldown";
    return ObjectEventPublishResult::Suppressed;
  }
  if (outcome.result == ObjectEventEnqueueResult::Failed) {
    LOG_ERROR << "Camera operator: observation outbox write failed; keeping "
                 "the event pending for retry";
    return ObjectEventPublishResult::Failed;
  }
  if (syncHook)
    syncHook("enqueue_post_commit");
  refreshCounters();
  wake_.notify_all();
  return outcome.inserted ? ObjectEventPublishResult::Recorded
                          : ObjectEventPublishResult::Duplicate;
}

void NatsObjectEventSink::refreshCounters()
{
  const std::scoped_lock refreshLock(refreshMutex_);
  const ObjectEventOutboxStats stats = outbox_.stats();
  const std::scoped_lock lock(countersMutex_);
  pendingCount_.store(stats.pending, std::memory_order_relaxed);
  sentCount_.store(stats.sent, std::memory_order_relaxed);
  overflowCount_.store(stats.overflowDropped, std::memory_order_relaxed);
  oldestPendingAtMs_.store(stats.oldestPendingAtMs, std::memory_order_relaxed);
}

void NatsObjectEventSink::reconcile()
{
  outbox_.purgeExpiredCooldowns(nowMs() - stream_retention::kRetentionMs);
  refreshCounters();
  if (!workerStarted_.exchange(true, std::memory_order_acq_rel))
    worker_ = std::thread([this]() { flushLoop(); });
}

void NatsObjectEventSink::requestStop()
{
  stopping_.store(true, std::memory_order_release);
  wake_.notify_all();
}

bool NatsObjectEventSink::drained() const
{
  return exited_.load(std::memory_order_acquire) ||
         !workerStarted_.load(std::memory_order_acquire);
}

Json::Value NatsObjectEventSink::health() const
{
  std::scoped_lock lock(countersMutex_);
  Json::Value status(Json::objectValue);
  status["pending"] = Json::Int64(pendingCount_.load(std::memory_order_relaxed));
  status["sent"] = Json::Int64(sentCount_.load(std::memory_order_relaxed));
  status["overflowDropped"] =
      Json::Int64(overflowCount_.load(std::memory_order_relaxed));
  const int64_t oldest = oldestPendingAtMs_.load(std::memory_order_relaxed);
  status["oldestPendingAgeS"] =
      oldest > 0 ? Json::Int64((nowMs() - oldest) / 1000) : Json::Int64(0);
  return status;
}

bool NatsObjectEventSink::flushOnce()
{
  if (!streamReady_.load(std::memory_order_acquire) && bus_->isConnected())
    streamReady_.store(camera_event_stream::ensure(
                           bus_, {.streamName = config_.streamName,
                                  .changeSubject = {},
                                  .objectSubject = config_.publishSubject}),
                       std::memory_order_release);

  const auto row = outbox_.nextPending();
  if (!row)
    return false;
  if (!bus_->isConnected())
    return false;

  const std::string subject =
      config_.publishSubject.empty()
          ? std::string(nats_subject::kCameraObjectDetected)
          : config_.publishSubject;
  if (bus_->publishWithMsgId({.subject = subject,
                              .payload = row->payload,
                              .msgId = row->eventId})) {
    if (!outbox_.markSent(row->eventId, nowMs())) {
      LOG_WARN << "Camera object outbox: " << row->eventId
               << " was stored but could not be marked sent; it stays pending";
      return false;
    }
    if (syncHook)
      syncHook("flush_post_mark_sent");
    refreshCounters();
    return true;
  }
  streamReady_.store(false, std::memory_order_relaxed);
  outbox_.recordAttempt(row->eventId);
  return false;
}

void NatsObjectEventSink::flushLoop()
{
  while (!stopping_.load(std::memory_order_acquire)) {
    bool progressed = false;
    try {
      progressed = flushOnce();
    }
    catch (const std::exception& e) {
      LOG_WARN << "Camera outbox: flush failed (" << e.what()
               << "); retrying";
    }
    const int64_t now = nowMs();
    if (now >= nextPurgeMs_) {
      nextPurgeMs_ = now + stream_retention::kSettledPurgeIntervalMs;
      try {
        const int64_t purged =
            outbox_.purgeSettled(now - stream_retention::kRetentionMs);
        if (purged > 0)
          LOG_INFO << "Camera object outbox: purged " << purged
                   << " settled row(s) past the stream's retention";
      }
      catch (const std::exception& e) {
        nextPurgeMs_ = now + stream_retention::kSettledPurgeRetryMs;
        LOG_WARN << "Camera object outbox: purge failed (" << e.what()
                 << "); the settled rows stay and the purge is retried";
      }
    }
    std::unique_lock lock(wakeMutex_);
    wake_.wait_for(lock,
                   std::chrono::milliseconds(progressed ? 50 : config_.retryMs));
  }
  exited_.store(true, std::memory_order_release);
}
