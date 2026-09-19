#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/wrapper/nats/nats-subject.hxx>

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

// Durable consumer over the guard-owned encounter stream. The inbox issues
// exactly one capture call per receipt: redeliveries after settlement are
// dropped, replays only follow an unsettled row, and poison dead-letters
// with a broker Term. stop() is idempotent and runs from the destructor;
// destroy only while the loop that started it is still running.
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

  // Unsubscribes, cancels the retry timer and waits in-flight handlers out.
  // Idempotent; call before tearing down the graph. Destroy only while the
  // loop that started it is still running.
  void stop();

  // Parses, validates and handles one raw payload for the broker settlement.
  drogon::Task<EncounterDisposition> handlePayload(const std::string& payload);

  // Receipts first, captures when new: Ack settles, Nak redelivers, Term
  // drops poison without redelivery.
  drogon::Task<EncounterDisposition> handle(
      const EncounterClosedEvent& event, const std::string& fingerprint);

private:
  bool trySubscribe();
  void scheduleSubscribeRetry();

  Dependencies dependencies_;
  Config config_;
  std::shared_ptr<EncounterLifecycle> lifecycle_;
  std::optional<uint64_t> subscription_;
  std::optional<uint64_t> retryTimer_;
};
