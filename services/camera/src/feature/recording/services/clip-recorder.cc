#include "clip-recorder.hxx"

#include <algorithm>
#include <auth/module-gate.hxx>
#include <auth/role-access.hxx>
#include <chrono>
#include <ctime>
#include <drogon/drogon.h>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/schemas/camera/camera-schema.hxx>
#include <shared/services/stream/ws-frame.hxx>
#include <sync/camera-change-sink.hxx>
#include <sync/resource-change.hxx>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
constexpr int kMaxSweepBatches = 50;
constexpr int64_t kSweepBatch = 200;
constexpr int64_t kPurgeDeletedSeconds = 7LL * 24 * 3600;
}

ClipRecorder& ClipRecorder::instance()
{
  static ClipRecorder recorder;
  return recorder;
}

std::shared_ptr<StreamHub::ISink> ClipRecorder::self()
{
  static std::shared_ptr<StreamHub::ISink> handle(&instance(),
                                                  [](StreamHub::ISink*) {});
  return handle;
}

int64_t ClipRecorder::steadyNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t ClipRecorder::epochNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void ClipRecorder::init(const RecordingConfig& config)
{
  config_ = config;
}

std::shared_ptr<ClipRecorder::CameraSlot>
ClipRecorder::slotForCamera(int64_t cameraId) const
{
  std::scoped_lock lock(slotsMutex_);
  const auto it = byCamera_.find(cameraId);
  return it == byCamera_.end() ? nullptr : it->second;
}

std::shared_ptr<ClipRecorder::CameraSlot>
ClipRecorder::slotForSub(uint16_t subId) const
{
  std::scoped_lock lock(slotsMutex_);
  const auto it = bySub_.find(subId);
  return it == bySub_.end() ? nullptr : it->second;
}

bool ClipRecorder::requestClip(const ClipRequestInput& input)
{
  if (stopping_.load(std::memory_order_acquire) || !config_.enabled)
    return false;
  const auto slot = slotForCamera(input.cameraId);
  if (!slot)
    return false;
  const int64_t atMs = steadyNowMs();
  const int64_t epochMs = input.atEpochMs > 0 ? input.atEpochMs : epochNowMs();
  std::scoped_lock lock(slot->mtx);
  if (!clip_recording::triggerAllowed(slot->mode, input.trigger))
    return false;
  return slot->builder.request(input.trigger, atMs, epochMs);
}

void ClipRecorder::notifyMotion(const CameraMotionReport& report)
{
  if (!report.moving || report.cameraId <= 0)
    return;
  static_cast<void>(requestClip({.cameraId = report.cameraId,
                                  .trigger = CameraClipTrigger::Motion,
                                  .atEpochMs = 0}));
}

void ClipRecorder::appendChunk(const std::shared_ptr<CameraSlot>& slot,
                               const uint8_t* payload, size_t len, bool keyframe,
                               int64_t atMs)
{
  std::optional<ReadyClip> ready;
  {
    std::scoped_lock lock(slot->mtx);
    slot->builder.push({.payload = payload, .len = len, .keyframe = keyframe,
                        .atMs = atMs});
    ready = slot->builder.takeReady();
  }
  if (!ready.has_value())
    return;
  if (ready->durationMs < config_.minClipMs)
    return;
  scheduleUpload({.cameraId = slot->cameraId,
                  .trigger = ready->trigger,
                  .codec = std::move(ready->codec),
                  .body = std::move(ready->body),
                  .startedAtMs = ready->startedAtMs,
                  .durationMs = ready->durationMs});
}

bool ClipRecorder::sendBinary(const uint8_t* data, size_t len)
{
  if (stopping_.load(std::memory_order_acquire))
    return true;
  const auto header = ws_frame::parse({.data = data, .len = len});
  if (!header.has_value())
    return true;
  const auto slot = slotForSub(header->subId);
  if (!slot)
    return true;
  const size_t payloadLen = len - ws_frame::kHeaderSize;
  const auto* payload = data + ws_frame::kHeaderSize;
  if (header->type == ws_frame::kTypeInit) {
    std::scoped_lock lock(slot->mtx);
    slot->builder.setInit(
        std::string(reinterpret_cast<const char*>(payload), payloadLen));
    return true;
  }
  if (header->type != ws_frame::kTypeMedia || payloadLen == 0)
    return true;
  appendChunk(slot, payload, payloadLen, header->keyframe, steadyNowMs());
  return true;
}

bool ClipRecorder::tryReserve(size_t bytes)
{
  static_cast<void>(bytes);
  return true;
}

int64_t ClipRecorder::release(int64_t bytes)
{
  return bytes;
}

void ClipRecorder::onClosed(const StreamClosedInput& input)
{
  const auto slot = slotForSub(input.subId);
  if (!slot)
    return;
  {
    std::scoped_lock lock(slotsMutex_);
    bySub_.erase(input.subId);
  }
  std::scoped_lock lock(slot->mtx);
  if (slot->subId.load(std::memory_order_acquire) != input.subId)
    return;
  slot->subId.store(0, std::memory_order_release);
  slot->builder.reset();
}

void ClipRecorder::attach(const std::shared_ptr<CameraSlot>& slot,
                          std::string& error)
{
  const uint16_t subId = StreamHub::instance().subscribe(
      {.sink = self(), .cameraId = slot->cameraId, .fastStart = true,
       .priority = false},
      error);
  if (subId == 0)
    return;
  {
    std::scoped_lock lock(slotsMutex_);
    bySub_[subId] = slot;
  }
  slot->subId.store(subId, std::memory_order_release);
}

void ClipRecorder::detach(const std::shared_ptr<CameraSlot>& slot)
{
  const uint16_t subId = slot->subId.exchange(0, std::memory_order_acq_rel);
  {
    std::scoped_lock lock(slot->mtx);
    slot->builder.reset();
  }
  if (subId == 0)
    return;
  {
    std::scoped_lock lock(slotsMutex_);
    bySub_.erase(subId);
  }
  StreamHub::instance().unsubscribe(subId, this);
}

void ClipRecorder::dropAll(const char* reason)
{
  std::vector<std::shared_ptr<CameraSlot>> slots;
  {
    std::scoped_lock lock(slotsMutex_);
    for (const auto& [cameraId, slot] : byCamera_)
      slots.push_back(slot);
  }
  for (const auto& slot : slots) {
    if (slot->subId.load(std::memory_order_acquire) == 0)
      continue;
    detach(slot);
    LOG_INFO << "Camera recording: camera " << slot->cameraId
             << " stopped recording (" << reason << ")";
  }
}

drogon::Task<void> ClipRecorder::reconcile()
{
  if (stopping_.load(std::memory_order_acquire))
    co_return;
  const bool surveillance = moduleGate().enabled(role_access::kSurveillanceModule);
  std::vector<CameraSchema> cameras;
  try {
    cameras = co_await cameraRepository_.findEnabled();
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera recording: the camera list could not be read: "
             << error.what();
    co_return;
  }
  std::unordered_set<int64_t> live;
  for (const auto& camera : cameras)
    live.insert(camera.id);
  std::vector<std::shared_ptr<CameraSlot>> stale;
  {
    std::scoped_lock lock(slotsMutex_);
    std::erase_if(byCamera_, [&stale, &live](const auto& entry) {
      if (live.contains(entry.first))
        return false;
      stale.push_back(entry.second);
      return true;
    });
  }
  for (const auto& slot : stale) {
    if (slot->subId.load(std::memory_order_acquire) != 0)
      detach(slot);
  }
  for (const auto& camera : cameras) {
    std::shared_ptr<CameraSlot> slot;
    {
      std::scoped_lock lock(slotsMutex_);
      auto& entry = byCamera_[camera.id];
      if (!entry) {
        entry = std::make_shared<CameraSlot>();
        entry->cameraId = camera.id;
        entry->builder.configure({.preRollMs = config_.preRollMs,
                                  .postRollMs = config_.postRollMs,
                                  .segmentMs = config_.segmentMs,
                                  .preRollBytes = config_.preRollBytes,
                                  .maxClipBytes = config_.maxClipBytes});
      }
      slot = entry;
    }
    {
      std::scoped_lock lock(slot->mtx);
      slot->mode = camera.recordMode;
    }
    const bool wanted = config_.enabled && surveillance &&
                        camera.recordMode != CameraRecordMode::Off;
    if (!wanted) {
      if (slot->subId.load(std::memory_order_acquire) != 0)
        detach(slot);
      continue;
    }
    if (slot->subId.load(std::memory_order_acquire) == 0) {
      std::string error;
      attach(slot, error);
      if (slot->subId.load(std::memory_order_acquire) == 0) {
        LOG_WARN << "Camera recording: camera " << camera.id
                 << " is not recording yet (" << error << ")";
        continue;
      }
    }
    if (camera.recordMode == CameraRecordMode::Continuous)
      static_cast<void>(requestClip({.cameraId = camera.id,
                                     .trigger = CameraClipTrigger::Continuous,
                                     .atEpochMs = 0}));
  }
  co_return;
}

void ClipRecorder::scheduleUpload(ClipUpload upload)
{
  drogon::app().getLoop()->queueInLoop(
      [this, upload = std::move(upload)]() mutable {
        if (stopping_.load(std::memory_order_acquire))
          return;
        drogon::async_run([this, upload = std::move(upload)]() mutable
                              -> drogon::Task<void> {
          co_await uploadClip(std::move(upload));
        });
      });
}

drogon::Task<int64_t> ClipRecorder::retentionSecondsFor(int64_t cameraId)
{
  constexpr int64_t kDaySeconds = 24LL * 3600;
  std::optional<int64_t> days;
  try {
    days = co_await clipRepository_.retentionDaysOf(cameraId);
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera recording: the retention of camera " << cameraId
             << " could not be read: " << error.what();
  }
  if (days.has_value() && *days <= 0)
    co_return 0;
  const int64_t configured = days.value_or(config_.retentionDays);
  co_return std::clamp<int64_t>(configured, config_.minRetentionDays,
                                config_.maxRetentionDays) *
      kDaySeconds;
}

drogon::Task<void> ClipRecorder::uploadClip(ClipUpload upload)
{
  const in_flight::Guard guard(inFlight_);
  try {
    if (!storage_.isConfigured()) {
      ++failed_;
      LOG_WARN << "Camera recording: object storage is not configured; the clip "
                  "of camera "
               << upload.cameraId << " is dropped";
      co_return;
    }
    const int64_t keepSeconds = co_await retentionSecondsFor(upload.cameraId);
    if (keepSeconds <= 0) {
      LOG_INFO << "Camera recording: camera " << upload.cameraId
               << " keeps no clips; the recording is dropped";
      co_return;
    }
    const int64_t startedAtSec = upload.startedAtMs / 1000;
    const std::string key = "cameras/" + std::to_string(upload.cameraId) +
                            "/clips/" + std::to_string(startedAtSec) + "-" +
                            std::to_string(upload.durationMs) + ".mp4";
    co_await storage_.put({.objectKey = key,
                           .body = upload.body,
                           .contentType = "video/mp4"});
    const int64_t now = std::time(nullptr);
    const auto row = co_await clipRepository_.create(
        {.cameraId = upload.cameraId,
         .trigger = upload.trigger,
         .objectKey = key,
         .contentType = "video/mp4",
         .codec = upload.codec,
         .startedAt = startedAtSec,
         .durationMs = upload.durationMs,
         .byteSize = static_cast<int64_t>(upload.body.size()),
         .expiresAt = now + keepSeconds});
    if (row.id <= 0)
      co_return;
    ++uploaded_;
    LOG_INFO << "Camera recording: camera " << upload.cameraId << " clip "
             << row.id << " holds " << upload.body.size() << " B for "
             << keepSeconds / 86400 << " day(s)";
    if (const auto* sink = camera_change::getSink()) {
      co_await sink->publishResource(
          {.scope = ResourceScope::Clips, .ids = {upload.cameraId}, .user = 0,
           .client = nullptr});
    }
  }
  catch (const std::exception& error) {
    ++failed_;
    LOG_WARN << "Camera recording: the clip of camera " << upload.cameraId
             << " could not be stored: " << error.what();
  }
  catch (...) {
    ++failed_;
    LOG_WARN << "Camera recording: the clip of camera " << upload.cameraId
             << " could not be stored";
  }
  co_return;
}

ClipRecorderStatus ClipRecorder::status() const
{
  ClipRecorderStatus out;
  std::vector<std::shared_ptr<CameraSlot>> slots;
  {
    std::scoped_lock lock(slotsMutex_);
    out.subscribed = static_cast<int64_t>(bySub_.size());
    for (const auto& [cameraId, slot] : byCamera_) {
      static_cast<void>(cameraId);
      slots.push_back(slot);
    }
  }
  for (const auto& slot : slots) {
    std::scoped_lock lock(slot->mtx);
    if (slot->builder.active())
      ++out.active;
  }
  out.uploaded = uploaded_.load(std::memory_order_acquire);
  out.failed = failed_.load(std::memory_order_acquire);
  return out;
}

drogon::Task<ClipSweepReport> ClipRecorder::runRetentionSweep(int64_t now)
{
  ClipSweepReport report;
  int64_t cursor = 0;
  for (int batch = 0;
       batch < kMaxSweepBatches && !stopping_.load(std::memory_order_acquire);
       ++batch) {
    const auto expired = co_await clipRepository_.findExpired(
        {.afterId = cursor,
         .now = now,
         .maxRetentionDays = config_.maxRetentionDays,
         .batch = kSweepBatch});
    if (expired.empty())
      break;
    for (const auto& clip : expired) {
      cursor = std::max(cursor, clip.id);
      bool removed = clip.objectKey.empty();
      if (!removed) {
        try {
          co_await storage_.remove(clip.objectKey);
          removed = true;
        }
        catch (const std::exception& error) {
          ++report.failed;
          LOG_WARN << "Camera recording: clip " << clip.id
                   << " could not be removed: " << error.what();
        }
      }
      if (!removed)
        continue;
      co_await clipRepository_.markDeleted({.id = clip.id, .deletedAt = now});
      ++report.removed;
    }
    if (static_cast<int64_t>(expired.size()) < kSweepBatch)
      break;
  }
  co_await clipRepository_.purgeDeleted(now - kPurgeDeletedSeconds);
  co_return report;
}

void ClipRecorder::sweepTick()
{
  drogon::async_run([this]() -> drogon::Task<void> {
    if (stopping_.load(std::memory_order_acquire))
      co_return;
    const in_flight::Guard guard(inFlight_);
    try {
      const auto report =
          co_await runRetentionSweep(static_cast<int64_t>(std::time(nullptr)));
      if (report.removed > 0 || report.failed > 0)
        LOG_INFO << "Camera recording: retention removed " << report.removed
                 << " clip(s), " << report.failed
                 << " failed and retried next sweep";
    }
    catch (const std::exception& error) {
      LOG_WARN << "Camera recording: the retention sweep failed: "
               << error.what();
    }
    catch (...) {
      LOG_WARN << "Camera recording: the retention sweep failed";
    }
    co_return;
  });
}

void ClipRecorder::start()
{
  bool expected = false;
  if (!starting_.compare_exchange_strong(expected, true))
    return;
  const auto loop = drogon::app().getLoop();
  rescanTimer_ = loop->runEvery(static_cast<double>(config_.rescanMs) / 1000.0,
                                [this]() {
                                  drogon::async_run(
                                      [this]() { return reconcile(); });
                                });
  sweepTimer_ = loop->runEvery(
      static_cast<double>(config_.sweepIntervalMs) / 1000.0,
      [this]() { sweepTick(); });
  sweepStartTimer_ = loop->runAfter(
      static_cast<double>(config_.sweepInitialDelayMs) / 1000.0,
      [this]() { sweepTick(); });
  drogon::async_run([this]() { return reconcile(); });
}

void ClipRecorder::requestStop()
{
  stopping_.store(true, std::memory_order_release);
  const auto invalidate = [](const std::optional<uint64_t>& timer) {
    if (timer.has_value() && drogon::app().isRunning())
      drogon::app().getLoop()->invalidateTimer(*timer);
  };
  invalidate(rescanTimer_);
  invalidate(sweepTimer_);
  invalidate(sweepStartTimer_);
  std::vector<std::shared_ptr<CameraSlot>> slots;
  {
    std::scoped_lock lock(slotsMutex_);
    for (const auto& [cameraId, slot] : byCamera_)
      slots.push_back(slot);
  }
  for (const auto& slot : slots)
    detach(slot);
}

bool ClipRecorder::drained() const
{
  return inFlight_.load(std::memory_order_acquire) == 0;
}
