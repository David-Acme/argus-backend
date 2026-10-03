#pragma once

#include <json/value.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class CameraNotificationPolicy
{
public:
  struct Config
  {
    int budgetPerHour{6};
    int silentStartHour{-1};
    int silentEndHour{-1};
    int64_t guardTimeoutMs{30000};
    double fallbackMinScoreMedian{0.3};
    int64_t fallbackMinDwellMs{1000};
    bool fallbackSuppressKnown{true};
    int fallbackRetentionDays{90};
  };

  enum class FallbackDecision
  {
    Notify,
    DropKnown,
    DropWeakScore,
    DropShortDwell
  };

  explicit CameraNotificationPolicy(Config config);

  const Config& config() const { return config_; }

  void reconfigure(const Config& config) { config_ = config; }

  bool shouldNotify(int64_t cameraId, int64_t nowMs);

  void countSuppressed(int64_t cameraId, const std::string& objectClass);

  std::vector<int64_t> trackedCameras() const;

  std::string takeDigest(int64_t cameraId, int64_t nowMs);

  bool guardReady(int64_t nowMs) const;

  void markGuardHeartbeat(int64_t nowMs);

  FallbackDecision fallbackDecision(const Json::Value& event) const;

  struct FallbackCounts
  {
    int64_t passed{0};
    int64_t droppedKnown{0};
    int64_t droppedWeakScore{0};
    int64_t droppedShortDwell{0};
  };

  FallbackCounts fallbackCounts() const;

  void countFallbackPass();
  void countFallbackDrop(FallbackDecision decision);

  static bool inSilentHours(const Config& config, int hour);

private:
  struct CameraWindow
  {
    int64_t windowStartMs{0};
    int notified{0};
    bool digestDue{false};
    std::map<std::string, int> suppressedByClass;
  };

  Config config_;
  std::map<int64_t, CameraWindow> windows_;
  int64_t lastGuardHeartbeatMs_{0};
  std::atomic<int64_t> fallbackPassed_{0};
  std::atomic<int64_t> fallbackDroppedKnown_{0};
  std::atomic<int64_t> fallbackDroppedWeakScore_{0};
  std::atomic<int64_t> fallbackDroppedShortDwell_{0};
};
