#pragma once

#include <drogon/utils/coroutine.h>
#include <memory>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <sync/camera-change-sink.hxx>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

class NatsBus;

// Camera-domain change funnel over argus.camera.v1.change: every emit and
// audit lands in the camera-owned outbox first, and a worker publishes from
// there, marking a row sent only after the JetStream PubAck. The subject's
// stream is the ARGUS_CAMERA one the object-event sink creates and heals.
class NatsCameraChangeSink : public CameraChangeSink
{
public:
  struct Config
  {
    int retryMs{500};
    // JetStream target; empty keeps the production camera change subject.
    std::string publishSubject;
  };

  NatsCameraChangeSink(std::shared_ptr<NatsBus> bus, Config config);
  ~NatsCameraChangeSink() override;
  NatsCameraChangeSink(const NatsCameraChangeSink&) = delete;
  NatsCameraChangeSink& operator=(const NatsCameraChangeSink&) = delete;

  [[nodiscard]] drogon::Task<void>
  emitModule(TableName table, const SocketEmitDto& body) const override;
  [[nodiscard]] drogon::Task<void>
  publishAudit(const ModuleAuditInput& input) const override;

  // Starts the publisher; call once after the schema is applied.
  void reconcile();

  // A payload the broker would refuse is not written: one such row would stop
  // every change queued behind it for ever. 256 KiB, past any real frame.
  static constexpr std::size_t kMaxPayloadBytes = std::size_t{256} * 1024;

private:
  [[nodiscard]] drogon::Task<void> enqueue(std::string eventId,
                                           std::string payloadJson) const;
  void flushLoop();
  bool flushOnce();

  std::shared_ptr<NatsBus> bus_;
  ChangeOutboxRepository outbox_;
  const Config config_;
  const std::string subject_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> workerStarted_{false};
  std::mutex wakeMutex_;
  mutable std::condition_variable wake_;
  std::thread worker_;
};
