#pragma once

#include <operator/object-event-sink.hxx>
#include <shared/repositories/object-event-outbox/object-event-outbox-repository.hxx>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <json/value.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class NatsBus;

// Enqueues observations into the camera-owned SQLite outbox and publishes them
// from a worker; a row is marked sent only after the JetStream PubAck, so a
// restart resumes whatever is still pending.
class NatsObjectEventSink final : public IObjectEventSink
{
public:
  struct Config
  {
    int64_t cooldownMs{1000};
    int64_t maxPending{5000};
    int retryMs{500};
    // Boot-scoped tag: person cooldown keys never collide across restarts.
    std::string sessionTag;
    // JetStream target; empty keeps the production camera stream/subject.
    std::string streamName;
    std::string publishSubject;
  };

  NatsObjectEventSink(std::shared_ptr<NatsBus> bus, Config config);
  ~NatsObjectEventSink() override;
  NatsObjectEventSink(const NatsObjectEventSink&) = delete;
  NatsObjectEventSink& operator=(const NatsObjectEventSink&) = delete;

  ObjectEventPublishResult
  publish(const ObjectDetectedEvent& event) override;

  // Non-blocking health view backed by counters, never by a DB query.
  Json::Value health() const;

  // Hydrates counters from SQLite and starts the publisher; call once after
  // the schema is applied.
  void reconcile();

  // Creates the JetStream stream when missing; true on success.
  static bool ensureStream(const std::shared_ptr<NatsBus>& bus,
                           const Config& config);

  // Test-only interleaving hook; empty in production.
  std::function<void(const std::string&)> syncHook;

private:
  void flushLoop();
  bool flushOnce();
  void refreshCounters();

  std::shared_ptr<NatsBus> bus_;
  ObjectEventOutboxRepository outbox_;
  const Config config_;
  const std::string sessionTag_;
  std::atomic<bool> stopping_{false};
  std::atomic<int64_t> nextSequence_{1};
  std::atomic<bool> streamReady_{false};
  std::atomic<bool> workerStarted_{false};
  std::atomic<int64_t> pendingCount_{0};
  std::atomic<int64_t> sentCount_{0};
  std::atomic<int64_t> overflowCount_{0};
  std::atomic<int64_t> oldestPendingAtMs_{0};
  mutable std::mutex countersMutex_;
  mutable std::mutex refreshMutex_;
  mutable std::mutex wakeMutex_;
  std::condition_variable wake_;
  std::thread worker_;
};
