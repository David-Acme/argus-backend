#pragma once

#include <config/guard-config.hxx>
#include <feature/proximity/repositories/proximity-beacon/proximity-beacon-repository.hxx>
#include <feature/proximity/repositories/proximity-receiver/proximity-receiver-repository.hxx>
#include <feature/proximity/repositories/proximity-sensor/proximity-sensor-repository.hxx>
#include <feature/proximity/services/beacon-token.hxx>
#include <feature/proximity/services/proximity-ports.hxx>
#include <feature/proximity/services/room-belief.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ProximityMeView
{
  bool enabled{false};
  std::optional<std::string> seed;
  int64_t rotateSeconds{900};
  int64_t receivers{0};
  bool located{true};
};

struct ProximityBleChange
{
  int64_t userId{0};
  bool enabled{false};
};

struct ProximityReceiverView
{
  ProximityReceiverRow row;
  bool online{false};
  std::optional<int64_t> lastSeenAt;
};

struct ProximitySensorView
{
  ProximitySensorRow row;
  bool online{false};
  std::optional<int64_t> lastSeenAt;
};

struct ProximityReceiverAdd
{
  std::string name;
  std::string room;
  ProximityReceiverKind kind{ProximityReceiverKind::Esphome};
  std::string host;
  int64_t port{80};
};

struct ProximitySensorAdd
{
  std::string name;
  std::string room;
  ProximitySensorModel model{ProximitySensorModel::Ld2410};
  ProximitySensorSource source{ProximitySensorSource::Esphome};
  std::string host;
  int64_t port{80};
  std::string entityId;
  std::optional<int64_t> cameraId;
  std::optional<double> panOffsetDeg;
};

struct ProximityVoiceEvidence
{
  int64_t userId{0};
  int64_t cameraId{0};
  double snrDb{0.0};
  bool voiceprint{false};
  int64_t atMs{0};
};

struct ProximityServiceInput
{
  const ProximityProbe* probe{nullptr};
  const ProximityWorld* world{nullptr};
  GuardProximityConfig config;
};

[[nodiscard]] RoomBeliefConfig roomBeliefConfigOf(const GuardProximityConfig& config);

[[nodiscard]] double bleStrength(int rssi, int64_t rssiFloor);

[[nodiscard]] double voiceStrength(const ProximityVoiceEvidence& evidence);

class ProximityService
{
public:
  explicit ProximityService(ProximityServiceInput input);

  ProximityService(const ProximityService&) = delete;
  ProximityService& operator=(const ProximityService&) = delete;

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;
  void refresh(const GuardProximityConfig& config);

  [[nodiscard]] drogon::Task<ProximityMeView> me(int64_t userId) const;
  [[nodiscard]] drogon::Task<ProximityMeView> setBle(ProximityBleChange change) const;

  [[nodiscard]] drogon::Task<std::vector<ProximityReceiverView>> receivers() const;
  [[nodiscard]] drogon::Task<ProximityReceiverView> addReceiver(ProximityReceiverAdd add) const;
  drogon::Task<void> removeReceiver(int64_t id) const;

  [[nodiscard]] drogon::Task<std::vector<ProximitySensorView>> sensors() const;
  [[nodiscard]] drogon::Task<ProximitySensorView> addSensor(ProximitySensorAdd add) const;
  drogon::Task<void> removeSensor(int64_t id) const;

  [[nodiscard]] drogon::Task<std::vector<RoomVerdict>> people() const;
  [[nodiscard]] drogon::Task<bool> reportVoice(ProximityVoiceEvidence evidence);

  drogon::Task<void> poll();

private:
  [[nodiscard]] GuardProximityConfig config() const;
  [[nodiscard]] drogon::Task<std::optional<bool>> located(int64_t userId) const;
  [[nodiscard]] drogon::Task<std::unordered_set<int64_t>> refreshLocated();
  [[nodiscard]] drogon::Task<std::unordered_map<int64_t, std::string>> refreshRooms();
  [[nodiscard]] drogon::Task<std::optional<int64_t>> userOf(int64_t personId);
  void observeLocated(const RoomEvidence& evidence);
  void markSeen(std::unordered_map<int64_t, int64_t>& seen, int64_t id, int64_t atMs);
  [[nodiscard]] std::optional<int64_t> seenAt(const std::unordered_map<int64_t, int64_t>& seen, int64_t id) const;
  [[nodiscard]] bool onlineAt(std::optional<int64_t> seenMs, int64_t nowMs) const;

  ProximityServiceInput input_;
  ProximityBeaconRepository beaconRepository_;
  ProximityReceiverRepository receiverRepository_;
  ProximitySensorRepository sensorRepository_;
  mutable RoomBelief belief_;
  mutable std::mutex mutex_;
  GuardProximityConfig config_;
  std::unordered_set<int64_t> located_;
  bool locatedKnown_{false};
  std::unordered_map<int64_t, std::string> rooms_;
  std::unordered_map<int64_t, std::optional<int64_t>> personUsers_;
  std::unordered_map<std::string, int64_t> boxSeenMs_;
  std::unordered_map<int64_t, int64_t> receiverSeenMs_;
  std::unordered_map<int64_t, int64_t> sensorSeenMs_;
  int64_t sightingsFromMs_{0};
  int64_t personCacheAtMs_{0};
  std::shared_ptr<std::function<void()>> scheduler_;
  std::atomic<bool> polling_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> started_{false};
};
