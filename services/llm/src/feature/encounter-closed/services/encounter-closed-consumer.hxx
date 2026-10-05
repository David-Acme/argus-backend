#pragma once

#include <atomic>
#include <drogon/utils/coroutine.h>
#include <nats/nats-subject.hxx>
#include <runtime/blocking-pool.hxx>

#include <cstdint>
#include <functional>
#include <json/value.h>
#include <optional>
#include <string>

class NatsBus;
class SqliteGraph;
class MemoryGraphRepository;
struct EncounterLifecycle;

struct EncounterClosedEvent
{
  std::string eventId;
  int64_t cameraId{0};
  int64_t personId{0};
  std::string grade;
  int64_t durationS{0};
  int64_t closedAt{0};

  static std::optional<EncounterClosedEvent> fromJson(const Json::Value& json);
};

struct EncounterCaptureInput
{
  int64_t cameraId{0};
  int64_t personId{0};
  std::string grade;
  int64_t durationS{0};
  int64_t closedAt{0};
  int64_t ownerUserId{0};
  std::string lang;
};

enum class EncounterDisposition : uint8_t
{
  Ack = 0,
  Nak,
  Term
};

class EncounterClosedConsumer
{
public:
  struct Dependencies
  {
    NatsBus* bus{nullptr};
    SqliteGraph* graph{nullptr};
    MemoryGraphRepository* repository{nullptr};
    std::function<int64_t(const EncounterCaptureInput&)> capture;
  };

  struct Config
  {
    std::string stream{nats_subject::kGuardStream};
    std::string durable{"argus-llm-encounters"};
    std::string subject{nats_subject::kGuardEncounterClosed};
    int maxDeliver{10};
    int poisonMaxAttempts{3};
    int64_t ownerUserId{0};
    std::string lang{"es"};
  };

  EncounterClosedConsumer(Dependencies dependencies, Config config);
  ~EncounterClosedConsumer();

  void start();

  void stop();

  void requestStop();

  [[nodiscard]] bool drained() const;

  drogon::Task<EncounterDisposition> handlePayload(const std::string& payload);

  drogon::Task<EncounterDisposition> handle(
      const EncounterClosedEvent& event, const std::string& fingerprint);

private:
  EncounterDisposition settlePayload(const std::string& payload);
  EncounterDisposition settleEvent(const EncounterClosedEvent& event,
                                   const std::string& fingerprint);
  bool trySubscribe();
  void scheduleSubscribeRetry();
  void purgeSettled(int64_t now);

  Dependencies dependencies_;
  Config config_;
  std::shared_ptr<EncounterLifecycle> lifecycle_;
  std::optional<uint64_t> subscription_;
  std::optional<uint64_t> retryTimer_;
  std::atomic<int64_t> nextPurgeAt_{0};
  BlockingStrand strand_{BlockingLane::Light};
};
