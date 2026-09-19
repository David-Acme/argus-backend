#include <operator/nats-object-event-sink.hxx>

#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <trantor/utils/Logger.h>

#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

// 7 days of server-side retention, in nanoseconds.
constexpr int64_t kJetStreamRetentionNs = 7LL * 24 * 60 * 60 * 1000000000;
constexpr int64_t kJetStreamDuplicatesNs = 2LL * 60 * 1000000000;

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace

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
} // namespace

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
  stopping_.store(true, std::memory_order_release);
  wake_.notify_all();
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
  // Serialized so a stale read can never overwrite a fresher snapshot.
  const std::lock_guard refreshLock(refreshMutex_);
  const ObjectEventOutboxStats stats = outbox_.stats();
  const std::lock_guard lock(countersMutex_);
  pendingCount_.store(stats.pending, std::memory_order_relaxed);
  sentCount_.store(stats.sent, std::memory_order_relaxed);
  overflowCount_.store(stats.overflowDropped, std::memory_order_relaxed);
  oldestPendingAtMs_.store(stats.oldestPendingAtMs, std::memory_order_relaxed);
}

void NatsObjectEventSink::reconcile()
{
  const int64_t retentionMs = 7LL * 24 * 60 * 60 * 1000;
  outbox_.purgeExpiredCooldowns(nowMs() - retentionMs);
  refreshCounters();
  if (!workerStarted_.exchange(true, std::memory_order_acq_rel))
    worker_ = std::thread([this]() { flushLoop(); });
}

Json::Value NatsObjectEventSink::health() const
{
  std::lock_guard lock(countersMutex_);
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
    streamReady_.store(ensureStream(bus_, config_), std::memory_order_release);

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
    if (outbox_.markSent(row->eventId, nowMs())) {
      if (syncHook)
        syncHook("flush_post_mark_sent");
      refreshCounters();
    }
    return true;
  }
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
    std::unique_lock lock(wakeMutex_);
    wake_.wait_for(lock,
                   std::chrono::milliseconds(progressed ? 50 : config_.retryMs),
                   [this]() {
                     return stopping_.load(std::memory_order_acquire);
                   });
  }
}

bool NatsObjectEventSink::ensureStream(const std::shared_ptr<NatsBus>& bus,
                                       const Config& config)
{
  if (!bus)
    return false;
  const std::string subject =
      config.publishSubject.empty()
          ? std::string(nats_subject::kCameraObjectDetected)
          : config.publishSubject;
  const std::string stream =
      config.streamName.empty() ? std::string("ARGUS_CAMERA")
                                : config.streamName;
  std::vector<std::string> subjects;
  if (config.streamName.empty() && config.publishSubject.empty())
    subjects = {"argus.camera.v1.change", subject};
  else
    subjects = {subject};
  if (bus->ensureStream({.name = stream,
                         .subjects = subjects,
                         .maxAgeNs = kJetStreamRetentionNs,
                         .duplicatesNs = kJetStreamDuplicatesNs})) {
    LOG_INFO << "Camera JetStream: stream " << stream
             << " ready (7d retention)";
    return true;
  }
  return false;
}
