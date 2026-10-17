#pragma once

#include <atomic>
#include <camera/camera-clip-trigger.hxx>
#include <camera/camera-record-mode.hxx>
#include <config/recording-config.hxx>
#include <cstddef>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/recording/repositories/camera-clip/camera-clip-repository.hxx>
#include <feature/recording/services/clip-builder.hxx>
#include <feature/recording/services/record-watchdog.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <memory>
#include <mutex>
#include <optional>
#include <shared/services/camera-motion/camera-motion-board.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <shared/utils/in-flight/in-flight.hxx>
#include <storage/s3-storage-service.hxx>
#include <string>
#include <unordered_map>

struct ClipRequestInput
{
  int64_t cameraId{0};
  CameraClipTrigger trigger{CameraClipTrigger::Event};
  int64_t atEpochMs{0};
};

struct ClipSweepReport
{
  int64_t removed{0};
  int64_t failed{0};
  int64_t stranded{0};
};

struct ClipRecorderStatus
{
  int64_t subscribed{0};
  int64_t active{0};
  int64_t uploaded{0};
  int64_t failed{0};
};

struct RecordingState
{
  bool paused{false};
  int64_t freeBytes{0};
  int64_t minFreeBytes{0};
};

class ClipRecorder final : public StreamHub::ISink
{
public:
  static ClipRecorder& instance();

  void init(const RecordingConfig& config);
  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

  bool requestClip(const ClipRequestInput& input);
  void notifyMotion(const CameraMotionReport& report);
  [[nodiscard]] ClipRecorderStatus status() const;
  [[nodiscard]] RecordingState state() const;

  drogon::Task<void> reconcile();
  drogon::Task<ClipSweepReport> runRetentionSweep(int64_t now);
  void dropAll(const char* reason);

  bool sendBinary(const uint8_t* data, size_t len) override;
  bool tryReserve(size_t bytes) override;
  int64_t release(int64_t bytes) override;
  void onSubscribed(uint16_t subId, int64_t cameraId) override;
  void onClosed(const StreamHub::StreamClosedInput& input) override;

private:
  ClipRecorder() = default;

  struct CameraSlot
  {
    std::mutex mtx;
    int64_t cameraId{0};
    std::atomic<uint16_t> subId{0};
    CameraRecordMode mode{CameraRecordMode::Events};
    bool announced{false};
    int64_t lastFragmentMs{0};
    record_watchdog::Interval keyframes;
    int64_t retryAtMs{0};
    int64_t retryBackoffMs{0};
    bool stalled{false};
    ClipBuilder builder;
  };

  struct ClipUpload
  {
    int64_t cameraId{0};
    CameraClipTrigger trigger{CameraClipTrigger::Event};
    std::string codec;
    std::string body;
    int64_t startedAtMs{0};
    int64_t durationMs{0};
  };

  static int64_t steadyNowMs();
  static int64_t epochNowMs();
  [[nodiscard]] static std::shared_ptr<StreamHub::ISink> self();

  [[nodiscard]] std::shared_ptr<CameraSlot> slotForCamera(int64_t cameraId) const;
  [[nodiscard]] std::shared_ptr<CameraSlot> slotForSub(uint16_t subId) const;
  void appendChunk(const std::shared_ptr<CameraSlot>& slot, const uint8_t* payload,
                   size_t len, bool keyframe, int64_t atMs);
  void scheduleUpload(ClipUpload upload);
  drogon::Task<void> uploadClip(ClipUpload upload);
  drogon::Task<int64_t> retentionSecondsFor(int64_t cameraId);
  void attach(const std::shared_ptr<CameraSlot>& slot, std::string& error);
  void detach(const std::shared_ptr<CameraSlot>& slot);
  void sweepTick();
  void watchdogTick();
  drogon::Task<void> floorTick();
  drogon::Task<int64_t> purgeOldestUntil(int64_t minBytes);
  drogon::Task<void> setPaused(bool paused, int64_t freeBytes);

  RecordingConfig config_;
  CameraRepository cameraRepository_;
  CameraClipRepository clipRepository_;
  S3StorageService storage_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> starting_{false};
  std::atomic<int64_t> inFlight_{0};
  mutable std::mutex slotsMutex_;
  std::unordered_map<int64_t, std::shared_ptr<CameraSlot>> byCamera_;
  std::unordered_map<uint16_t, std::shared_ptr<CameraSlot>> bySub_;
  std::atomic<int64_t> uploaded_{0};
  std::atomic<int64_t> failed_{0};
  std::optional<uint64_t> rescanTimer_;
  std::optional<uint64_t> sweepTimer_;
  std::optional<uint64_t> sweepStartTimer_;
  std::optional<uint64_t> watchdogTimer_;
  std::optional<uint64_t> floorTimer_;
  std::atomic<bool> paused_{false};
  std::atomic<int64_t> freeBytes_{0};
  std::atomic<int64_t> minFreeBytes_{0};
};
