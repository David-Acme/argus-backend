#include "nats-camera-change-sink.hxx"

#include <camera/camera-errors.hxx>
#include <chrono>
#include <errors/response-exception.hxx>
#include <exception>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
#include <shared/services/event-stream/event-stream.hxx>
#include <sync/module-audit-event.hxx>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
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
constexpr int kDrainBatch = 64;
constexpr int kProgressMs = 50;
}

NatsCameraChangeSink::NatsCameraChangeSink(std::shared_ptr<NatsBus> bus,
                                           Config config)
    : bus_(std::move(bus)),
      config_(std::move(config)),
      subject_(config_.publishSubject.empty()
                   ? std::string(nats_subject::kCameraChange)
                   : config_.publishSubject)
{
}

NatsCameraChangeSink::~NatsCameraChangeSink()
{
  requestStop();
  if (worker_.joinable())
    worker_.join();
}

drogon::Task<void>
NatsCameraChangeSink::emitModule(const ModuleEmitInput& input) const
{
  const Json::Value& recordId = input.body.obj["id"];
  if (!recordId.isIntegral()) {
    LOG_ERROR << "Camera change outbox: an emit of "
              << tableNameToString(input.table)
              << " carried no record id; the write is refused";
    throw ResponseException(CameraErrors::ChangeNotRecorded);
  }
  const std::string payload = json_util::toString(input.body.toJson());
  const std::string id =
      change_outbox_key::eventId({.table = tableNameToString(input.table),
                                  .recordId = recordId.asInt64(),
                                  .discriminator = payload});
  co_await enqueue(
      {.eventId = id, .payload = payload, .client = input.client});
}

drogon::Task<void>
NatsCameraChangeSink::publishAudit(const ModuleAuditInput& input) const
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
  co_await enqueue(
      {.eventId = id, .payload = payload, .client = input.client});
}

drogon::Task<void>
NatsCameraChangeSink::enqueue(ChangeOutboxEnqueueInput input) const
{
  if (input.payload.size() > kMaxPayloadBytes) {
    LOG_ERROR << "Camera change outbox: " << input.eventId << " carries "
              << input.payload.size()
              << " bytes, past the broker's message budget; the write is "
                 "refused";
    throw ResponseException(CameraErrors::ChangeNotRecorded);
  }
  input.fingerprint = change_outbox_key::fingerprintJson(input.payload);
  input.at = nowMs();
  co_await outbox_.enqueue(input);
  wake_.notify_all();
}

void NatsCameraChangeSink::reconcile()
{
  if (!workerStarted_.exchange(true, std::memory_order_acq_rel))
    worker_ = std::thread([this]() { flushLoop(); });
}

void NatsCameraChangeSink::requestStop()
{
  stopping_.store(true, std::memory_order_release);
  wake_.notify_all();
}

bool NatsCameraChangeSink::drained() const
{
  return exited_.load(std::memory_order_acquire) ||
         !workerStarted_.load(std::memory_order_acquire);
}

bool NatsCameraChangeSink::ensureStream() const
{
  return camera_event_stream::ensure(
      bus_, {.streamName = config_.streamName,
             .changeSubject = config_.publishSubject,
             .objectSubject = {}});
}

bool NatsCameraChangeSink::flush(const ChangeOutboxRow& row)
{
  if (!bus_ || !bus_->isConnected())
    return false;
  if (!streamReady_.load(std::memory_order_acquire))
    streamReady_.store(ensureStream(), std::memory_order_release);

  if (bus_->publishWithMsgId(
          {.subject = subject_, .payload = row.payload, .msgId = row.eventId})) {
    if (!outbox_.markSent(row.eventId, nowMs())) {
      LOG_WARN << "Camera change outbox: " << row.eventId
               << " was stored but could not be marked sent; it stays pending";
      return false;
    }
    return true;
  }
  streamReady_.store(false, std::memory_order_relaxed);
  static_cast<void>(outbox_.recordAttempt(row.eventId));
  const int64_t attempts = row.attempts + 1;
  if (attempts <= 1 || attempts % kStuckLogEvery == 0)
    LOG_WARN << "Camera change outbox: " << row.eventId
             << " is still unpublished after " << attempts
             << " attempts; every later change waits behind it";
  return false;
}

void NatsCameraChangeSink::flushLoop()
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
      LOG_WARN << "Camera change outbox: flush failed (" << e.what()
               << "); retrying";
    }
    std::unique_lock lock(wakeMutex_);
    wake_.wait_for(lock,
                   std::chrono::milliseconds(progressed ? kProgressMs
                                                        : config_.retryMs));
  }
  exited_.store(true, std::memory_order_release);
}
