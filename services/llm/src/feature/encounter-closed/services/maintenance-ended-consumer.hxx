#pragma once

#include <drogon/utils/coroutine.h>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/blocking-pool.hxx>

#include <atomic>
#include <cstdint>
#include <functional>
#include <json/value.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class SqliteGraph;
class MemoryGraphRepository;
struct MaintenanceLifecycle;

struct MaintenanceEndedEvent
{
  std::string eventId;
  int64_t maintenanceId{0};
  std::string environment;
  std::vector<std::string> cameras;
  int64_t declaredBy{0};
  std::string declaredName;
  std::string endReason;
  int64_t startedAt{0};
  int64_t endedAt{0};

  static std::optional<MaintenanceEndedEvent> fromJson(const Json::Value& json);
};

struct MaintenanceCaptureInput
{
  const MaintenanceEndedEvent& event;
  int64_t ownerUserId{0};
  std::string lang;
};

struct MaintenanceSummaryInput
{
  const MaintenanceEndedEvent& event;
  std::string lang;
  int utcOffsetSeconds{0};
};

namespace maintenance_episode
{
[[nodiscard]] std::string summary(const MaintenanceSummaryInput& input);
}

enum class MaintenanceDisposition : uint8_t
{
  Ack = 0,
  Nak,
  Term
};

struct MaintenanceDelivery
{
  bool alive{false};
  const std::string& payload;
  const NatsBus::DurableSettlement& settlement;
};

class MaintenanceEndedConsumer
{
public:
  struct Dependencies
  {
    NatsBus* bus{nullptr};
    SqliteGraph* graph{nullptr};
    MemoryGraphRepository* repository{nullptr};
    std::function<int64_t(const MaintenanceCaptureInput&)> capture;
  };

  struct Config
  {
    std::string stream{nats_subject::kGuardStream};
    std::string durable{"argus-llm-maintenance"};
    std::string subject{nats_subject::kGuardMaintenanceEnded};
    int maxDeliver{10};
    int poisonMaxAttempts{3};
    int64_t ownerUserId{0};
    std::string lang{"es"};
  };

  MaintenanceEndedConsumer(Dependencies dependencies, Config config);
  ~MaintenanceEndedConsumer();

  MaintenanceEndedConsumer(const MaintenanceEndedConsumer&) = delete;
  MaintenanceEndedConsumer& operator=(const MaintenanceEndedConsumer&) = delete;

  void start();

  void stop();

  void requestStop();

  [[nodiscard]] bool drained() const;

  drogon::Task<MaintenanceDisposition> handlePayload(const std::string& payload);

private:
  MaintenanceDisposition settlePayload(const std::string& payload);
  MaintenanceDisposition settleEvent(const MaintenanceEndedEvent& event, const std::string& fingerprint);
  void settleDelivery(const MaintenanceDelivery& delivery);
  bool trySubscribe();
  void scheduleSubscribeRetry();

  Dependencies dependencies_;
  Config config_;
  std::shared_ptr<MaintenanceLifecycle> lifecycle_;
  std::optional<uint64_t> subscription_;
  std::optional<uint64_t> retryTimer_;
  BlockingStrand strand_{BlockingLane::Light};
};
